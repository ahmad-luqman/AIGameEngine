#include "Basalt/Renderer/SceneRenderer.h"

#include "Basalt/Animation/AnimatedModel.h"
#include "Basalt/Animation/AnimationPose.h"
#include "Basalt/Animation/AnimationSystem.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/Profiler.h"
#include "Basalt/Math/Random.h"
#include "Basalt/Particles/ParticleSystem.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Renderer/DebugLineRenderer.h"
#include "Basalt/Renderer/Fsr1Upscaler.h"
#include "Basalt/Renderer/LightClusters.h"
#include "Basalt/Renderer/LocalShadows.h"
#include "Basalt/Renderer/PostProcessStack.h"
#include "Basalt/Renderer/RenderMath.h"
#include "Basalt/Renderer/SceneLights.h"
#include "Basalt/Renderer/ShaderUtils.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <nvrhi/utils.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <map>
#include <variant>
#include <unordered_set>

namespace Basalt {

	namespace {

		// One mask bit lets the outline pass cover every selected entity without extra draw passes.
		constexpr uint32_t SelectedTokenBit = 0x80000000u;
		constexpr uint32_t MaxPickToken = SelectedTokenBit - 1;
		static_assert(MaxPickToken < SelectedTokenBit && (MaxPickToken & SelectedTokenBit) == 0);

		struct OutlinePush
		{
			glm::uvec2 RenderSize, OutputSize;
			uint32_t SelectionMask, Padding;
		};
		static_assert(sizeof(OutlinePush) == 24);

		// Batch readbacks share one submission and device wait, including the ID/depth pair.
		struct ReadbackRegion
		{
			nvrhi::ITexture* Texture;
			nvrhi::TextureSlice Slice;
			std::vector<uint8_t> Pixels;
		};

		bool ReadTextureRegions(nvrhi::IDevice* device, std::vector<ReadbackRegion>& regions)
		{
			auto commandList = device->createCommandList();
			if (!commandList)
			{
				BS_CORE_ERROR("SceneRenderer: failed to create readback command list");
				return false;
			}
			std::vector<nvrhi::StagingTextureHandle> staging;
			for (const auto& region : regions)
			{
				nvrhi::TextureDesc desc;
				desc.width = region.Slice.width;
				desc.height = region.Slice.height;
				desc.format = region.Texture->getDesc().format;
				desc.debugName = "ViewportReadback";
				staging.push_back(device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read));
				if (!staging.back())
				{
					BS_CORE_ERROR("SceneRenderer: failed to allocate texture readback");
					return false;
				}
			}
			commandList->open();
			for (size_t i = 0; i < regions.size(); i++)
				commandList->copyTexture(staging[i], nvrhi::TextureSlice(), regions[i].Texture, regions[i].Slice);
			commandList->close();
			device->executeCommandList(commandList);
			device->waitForIdle();
			for (size_t i = 0; i < regions.size(); i++)
			{
				auto& region = regions[i];
				size_t rowPitch = 0;
				const auto* data = static_cast<const uint8_t*>(device->mapStagingTexture(staging[i], nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
				if (!data)
				{
					BS_CORE_ERROR("SceneRenderer: failed to map texture readback");
					return false;
				}
				const size_t rowBytes = static_cast<size_t>(region.Slice.width) * nvrhi::getFormatInfo(region.Texture->getDesc().format).bytesPerBlock;
				region.Pixels.resize(rowBytes * region.Slice.height);
				for (uint32_t y = 0; y < region.Slice.height; y++)
					std::memcpy(region.Pixels.data() + y * rowBytes, data + y * rowPitch, rowBytes);
				device->unmapStagingTexture(staging[i]);
			}
			return true;
		}

		constexpr uint32_t MaxCascades = BS_SHADOW_CASCADE_COUNT;
		constexpr uint32_t ShadowMapSize = 2048;
		constexpr uint32_t EnvironmentSize = 512;
		constexpr uint32_t IrradianceSize = 32;
		constexpr uint32_t PrefilteredSize = 128;
		constexpr uint32_t PrefilteredMips = 6;
		constexpr uint32_t BRDFLutSize = 256;
		constexpr uint32_t SSAOKernelSize = 24;
		constexpr uint32_t MaxLights = MaxSceneLights;
		constexpr uint64_t MaterialSetRetentionFrames = 120;
		constexpr uint32_t ParticleTextureSize = 64;
		// Billboards drawn per frame, at most (a 32 MB upload; the buffer grows to twice that); beyond it whole
		// emitters are skipped.
		constexpr size_t MaxFrameParticles = 1000000;
		// Set 1 of the particle pipelines (Particle.vert/.frag; keep in sync).
		constexpr uint32_t ParticleBufferSlot = 0;
		constexpr uint32_t ParticleTextureSlot = 1;
		constexpr uint32_t ParticleSamplerSlot = 2;

		// GPU profiler passes (debug markers too), in recording order. Unscoped so the values index the
		// profiler's pass list directly; the names below must match the order.
		enum ScenePass : uint32_t
		{
			PassEnvironment, // BRDF LUT and IBL bake: only on the first frame and when the sky changes
			PassUploads,     // per-frame buffers (not the SSAO constants, written in their pass), streamed meshes and textures
			PassShadows,
			PassPrepass,
			PassSSAO,
			PassClusters,
			PassLighting,
			PassPostProcess,
			PassTonemap,
			PassOverlayDepth,
			PassDebugLines,
			PassCount
		};
		constexpr const char* ScenePassNames[] = { "Environment", "Uploads", "Shadows", "Prepass", "SSAO", "Clusters", "Lighting", "PostProcess", "Tonemap", "OverlayDepth", "DebugLines" };
		// A C array, not std::array<..., PassCount>, which would accept too few names (filling the rest with null).
		static_assert(std::size(ScenePassNames) == PassCount, "one name per ScenePass");

		bool CreateLocalShadowTarget(nvrhi::IDevice* device, uint32_t size, nvrhi::TextureHandle& texture,
									 nvrhi::FramebufferHandle& framebuffer, bool reportFailure)
		{
			nvrhi::TextureDesc desc;
			desc.width = desc.height = size;
			desc.format = nvrhi::Format::D32;
			desc.isRenderTarget = true;
			desc.debugName = size == 1 ? "LocalShadowFallback" : "LocalShadowAtlas";
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.setClearValue(nvrhi::Color(1.0f));
			auto target = device->createTexture(desc);
			if (!target)
			{
				if (reportFailure)
					BS_CORE_ERROR("SceneRenderer: failed to create {}x{} local shadow texture", size, size);
				return false;
			}
			auto targetFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().setDepthAttachment(target));
			if (!targetFramebuffer)
			{
				if (reportFailure)
					BS_CORE_ERROR("SceneRenderer: failed to create local shadow framebuffer ({}x{})", size, size);
				return false;
			}
			texture = target;
			framebuffer = targetFramebuffer;
			return true;
		}

		// --- GPU data layouts (must match Basalt/Shaders/Include/Common.glsl) ------------------------

		struct GpuDirectionalLight
		{
			glm::vec4 Direction;
			glm::vec4 Color;
			glm::vec4 ShadowParams;
		};

		struct GpuViewConstants
		{
			glm::mat4 View;
			glm::mat4 Projection;
			glm::mat4 ViewProjection;
			glm::mat4 InverseView;
			glm::mat4 InverseProjection;
			glm::mat4 CascadeViewProjection[MaxCascades];
			glm::vec4 CascadeSplits;
			glm::vec4 CameraPosition;
			glm::vec4 ViewportSize;
			glm::vec4 AmbientColor;
			glm::vec4 EnvironmentParams;
			glm::vec4 CameraClip;
			GpuDirectionalLight Sun;
			glm::uvec4 ClusterGrid;
			glm::vec4 ClusterDepths;
			glm::vec4 Quality;
		};
		static_assert(sizeof(GpuViewConstants) == 768, "GpuViewConstants must match the std140 ViewConstants block");

		struct GpuDrawData
		{
			glm::mat4 Model;
			glm::mat4 Normal;
			glm::uvec4 Indices;
		};
		static_assert(sizeof(GpuDrawData) == 144);

		struct GpuMaterialData
		{
			glm::vec4 AlbedoColor;
			glm::vec4 Emissive;
			glm::vec4 Params;
		};
		static_assert(sizeof(GpuMaterialData) == 48);

		struct GpuSSAOConstants
		{
			glm::mat4 Projection;
			glm::mat4 InverseProjection;
			glm::vec4 Kernel[32];
			glm::vec4 Params;
			glm::vec4 ScreenSize;
			glm::vec4 UVScale;
		};

		struct DrawPush
		{
			uint32_t DrawIndex;
			uint32_t ShadowViewIndex; // Cascades first, followed by local atlas maps.
		};

		// Matches TonemapPush in Tonemap.frag.
		struct TonemapPush
		{
			float Exposure;
			uint32_t Operator;
			uint32_t DebugView;
			uint32_t OutputMode;
			glm::vec2 UVScale;
		};
		static_assert(sizeof(TonemapPush) == 24);

		// Matches BlurPush in SSAOBlur.frag.
		struct BlurPush
		{
			glm::vec2 Direction;
			float Near;
			float Far;
			float Orthographic;
			float Padding;
			glm::vec2 UVScale;
		};
		static_assert(sizeof(BlurPush) == 32);

		// The texture + sampler layout's push constants: DepthUpsample.frag's UVScale (Blit.frag ignores them).
		constexpr uint32_t TextureSamplerPushSize = sizeof(glm::vec2);
		static_assert(TextureSamplerPushSize == 8);

		struct SkyPush
		{
			glm::vec4 ZenithColor;
			glm::vec4 HorizonColor;
			glm::vec4 GroundColor;
			glm::vec4 SunDirection;
		};

		enum MaterialFlags : uint32_t
		{
			MaterialHasAlbedoMap = 1,
			MaterialHasNormalMap = 2,
			MaterialHasMetallicRoughnessMap = 4,
			MaterialHasOcclusionMap = 8,
			MaterialHasEmissiveMap = 16
		};

		struct DrawItem
		{
			const GpuMesh* Mesh = nullptr;
			uint32_t IndexCount = 0;
			uint32_t BaseIndex = 0;
			uint32_t BaseVertex = 0;
			uint32_t DrawIndex = 0;
			nvrhi::IBindingSet* MaterialSet = nullptr;
			bool Blend = false;
			bool DoubleSided = false;
			bool CastShadows = true;
			// Alpha-tested (glTF Mask) materials need the alpha-testing shadow pipeline.
			bool AlphaTested = false;
			float ViewDepth = 0.0f;
			// Valid only when visible to the camera.
			AABB WorldBounds;
			AABB ShadowBounds;
		};

		// One emitter's batch with the binding set of its texture (a strong handle, so the draw never depends
		// on the cache entry).
		struct ParticleDraw
		{
			const ParticleBatch* Batch = nullptr;
			nvrhi::BindingSetHandle Set;
		};

		// One overload set from several lambdas, for std::visit.
		template<typename... Lambdas>
		struct Overloaded : Lambdas...
		{
			using Lambdas::operator()...;
		};

		// Blended meshes and particle batches share one back-to-front order. The pointers stay valid while the
		// frame draws: the item, draw and batch lists are complete before this list is built.
		struct TransparentDraw
		{
			float ViewDepth = 0.0f;
			std::variant<const DrawItem*, const ParticleDraw*> Source;
		};

		// White with a smooth round alpha falloff: particles without a texture look like soft dots.
		std::vector<uint8_t> MakeSoftDiscTexture(uint32_t size)
		{
			std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
			for (uint32_t y = 0; y < size; y++)
			{
				for (uint32_t x = 0; x < size; x++)
				{
					// Distance from the center in texel centers, 1 at the edge of the inscribed circle.
					const glm::vec2 offset((static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f, (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f);
					const float falloff = std::clamp(1.0f - glm::length(offset), 0.0f, 1.0f);
					// Smoothstep keeps the center bright and the rim free of a visible edge.
					const float alpha = falloff * falloff * (3.0f - 2.0f * falloff);
					uint8_t* pixel = &pixels[(static_cast<size_t>(y) * size + x) * 4];
					pixel[0] = pixel[1] = pixel[2] = 255;
					pixel[3] = static_cast<uint8_t>(std::lround(alpha * 255.0f));
				}
			}
			return pixels;
		}

		nvrhi::BufferHandle EnsureStructuredBuffer(nvrhi::IDevice* device, nvrhi::BufferHandle buffer, size_t requiredBytes, uint32_t stride, const char* name)
		{
			if (buffer && buffer->getDesc().byteSize >= requiredBytes)
				return buffer;
			nvrhi::BufferDesc desc;
			desc.byteSize = std::max<size_t>(requiredBytes * 2, static_cast<size_t>(stride) * 64);
			desc.structStride = stride;
			desc.debugName = name;
			desc.canHaveRawViews = false;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			return device->createBuffer(desc);
		}

	}

	struct SceneRenderer::Impl
	{
		nvrhi::CommandListHandle CommandList;
		DebugLineRenderer Lines;
		nvrhi::TextureHandle EntityIds, PickDepth;
		nvrhi::FramebufferHandle PickFramebuffer;
		nvrhi::ShaderHandle DepthNormalPickPS, PickPS, OutlinePS;
		nvrhi::BindingLayoutHandle OutlineLayout;
		nvrhi::BindingSetHandle OutlineSet;
		nvrhi::GraphicsPipelineHandle PickPipeline[2], OutlinePipeline;
		std::vector<UUID> PickEntities;
		bool PickReady = false;

		// Shaders
		nvrhi::ShaderHandle ShadowMaskedVS, ShadowMaskedPS;
		nvrhi::ShaderHandle MeshVS, DepthNormalPS, PbrPS, ShadowVS, FullscreenVS, SsaoPS, SsaoBlurPS, SkyboxVS, SkyboxPS, TonemapPS, BlitPS, DepthUpsamplePS;
		nvrhi::ShaderHandle EquirectCS, SkyCS, IrradianceCS, PrefilterCS, BrdfCS, DownsampleCS;
		nvrhi::ShaderHandle ParticleVS, ParticlePS;

		// Layouts
		nvrhi::InputLayoutHandle MeshInputLayout;
		nvrhi::InputLayoutHandle ShadowInputLayout;
		nvrhi::InputLayoutHandle ShadowMaskedInputLayout;
		nvrhi::BindingLayoutHandle FrameLayout;
		nvrhi::BindingLayoutHandle ShadowFrameLayout;
		nvrhi::BindingLayoutHandle MaterialLayout;
		nvrhi::BindingLayoutHandle SsaoLayout;
		nvrhi::BindingLayoutHandle BlurLayout;
		nvrhi::BindingLayoutHandle TextureSamplerLayout; // blit, depth upsample: texture(0), sampler(1)
		nvrhi::BindingLayoutHandle TonemapLayout;        // HDR(0), point sampler(1), SSAO(2), normals(3), depth(4), HDR sampler(5)
		nvrhi::BindingLayoutHandle ComputeLayout;        // IBL: texture(0), sampler(1), image(2)
		nvrhi::BindingLayoutHandle ParticleLayout;       // set 1: Particle*Slot constants

		// Samplers
		nvrhi::SamplerHandle EquirectSampler, LinearClamp, LinearWrapAniso, PointClamp, PointWrap, ShadowCompare;

		// Constant GPU resources
		nvrhi::BufferHandle ViewConstantsBuffer;
		MemoryAllocation ViewMemory{ MemoryCategory::GpuBuffers };
		MemoryAllocation SsaoMemory{ MemoryCategory::GpuBuffers };
		MemoryAllocation DrawMemory{ MemoryCategory::GpuBuffers };
		MemoryAllocation MaterialMemory{ MemoryCategory::GpuBuffers };
		MemoryAllocation LocalShadowMemory{ MemoryCategory::GpuBuffers };
		MemoryAllocation ParticleMemory{ MemoryCategory::GpuBuffers };
		nvrhi::BufferHandle SsaoConstantsBuffer;
		nvrhi::BufferHandle DrawBuffer, MaterialBuffer, LightBuffer;
		LightClusters Clusters;
		nvrhi::TextureHandle ShadowMap;
		nvrhi::TextureHandle LocalShadowAtlas;
		nvrhi::FramebufferHandle LocalShadowFramebuffer;
		nvrhi::BufferHandle LocalShadowBuffer;
		nvrhi::GraphicsPipelineHandle LocalShadowPipeline, LocalShadowMaskedPipeline;
		std::array<nvrhi::FramebufferHandle, MaxCascades> ShadowFramebuffers;
		nvrhi::TextureHandle SsaoNoise;
		nvrhi::TextureHandle BrdfLut;
		bool BrdfLutReady = false;
		nvrhi::BufferHandle ParticleBuffer;
		nvrhi::TextureHandle ParticleTexture; // built-in soft disc
		GpuSSAOConstants SsaoConstants{};

		// Environment (IBL)
		struct Environment
		{
			nvrhi::TextureHandle Cube;
			nvrhi::TextureHandle Irradiance;
			nvrhi::TextureHandle Prefiltered;
		};
		// Identity of the baked environment: map path, source object and (procedural sky) sun direction.
		std::string EnvironmentKey = "\x01"; // never a valid key: forces the first build
		Environment Env;

		nvrhi::BindingSetHandle CreateTonemapSet(nvrhi::IDevice* device, nvrhi::ITexture* source, nvrhi::ITexture* depth) const
		{
			return device->createBindingSet(nvrhi::BindingSetDesc()
												.addItem(nvrhi::BindingSetItem::PushConstants(PushConstantSlot, sizeof(TonemapPush)))
												.addItem(nvrhi::BindingSetItem::Texture_SRV(0, source))
												.addItem(nvrhi::BindingSetItem::Sampler(1, PointClamp))
												.addItem(nvrhi::BindingSetItem::Texture_SRV(2, SsaoTexture))
												.addItem(nvrhi::BindingSetItem::Texture_SRV(3, NormalTexture))
												.addItem(nvrhi::BindingSetItem::Texture_SRV(4, depth))
												// The shader uses explicit texelFetch sampling.
												.addItem(nvrhi::BindingSetItem::Sampler(5, PointClamp)),
											TonemapLayout);
		}

		PostProcessStack PostProcess;
		nvrhi::GraphicsPipelineHandle FxaaTonemapPipeline;
		nvrhi::TextureHandle TonemapSource;
		std::string InvalidPostProcessError;
		UUID InvalidPostProcessEntity = 0;
		bool TonemapSetFailed = false;
		bool FxaaTonemapFailed = false;
		uint64_t ExposureRevision = 0;
		UUID ExposureComponent = 0;
		bool PostProcessWasActive = false;
		Fsr1Upscaler Upscaler;
		nvrhi::GraphicsPipelineHandle FsrTonemapPipeline;
		bool FsrTonemapFailed = false;
		bool FsrRequested = false;

		// Size-dependent targets. The scene renders into the top-left render size of the targets; the output
		// (and OverlayDepth) has the viewport size. OverlayDepth exists only when the two can differ.
		nvrhi::TextureHandle NormalTexture, HdrTexture, SsaoTexture, SsaoBlurTexture, OverlayDepth;
		nvrhi::FramebufferHandle PrepassFramebuffer, LightingFramebuffer, SsaoFramebuffer, SsaoBlurFramebuffer, OutputFramebuffer, OverlayDepthFramebuffer;

		// Pipelines (created lazily per framebuffer layout where needed)
		nvrhi::GraphicsPipelineHandle PrepassPipeline[2]; // [doubleSided]
		nvrhi::GraphicsPipelineHandle OpaquePipeline[2];
		nvrhi::GraphicsPipelineHandle TransparentPipeline[2];
		nvrhi::GraphicsPipelineHandle ParticlePipeline[ParticleBlendModeCount];
		nvrhi::GraphicsPipelineHandle ShadowPipeline;
		nvrhi::GraphicsPipelineHandle ShadowMaskedPipeline;
		nvrhi::GraphicsPipelineHandle SkyboxPipeline;
		nvrhi::GraphicsPipelineHandle SsaoPipeline;
		nvrhi::GraphicsPipelineHandle SsaoBlurPipeline;
		nvrhi::GraphicsPipelineHandle TonemapPipeline;
		nvrhi::GraphicsPipelineHandle DepthUpsamplePipeline;
		std::unordered_map<std::string, nvrhi::GraphicsPipelineHandle> BlitPipelines;
		nvrhi::ComputePipelineHandle EquirectPipeline, SkyPipeline, IrradiancePipeline, PrefilterPipeline, BrdfPipeline, DownsamplePipeline;

		// Binding sets
		nvrhi::BindingSetHandle FrameSet;
		nvrhi::BindingSetHandle ShadowFrameSet;
		nvrhi::BindingSetHandle SsaoSet, BlurHorizontalSet, BlurVerticalSet, TonemapSet, BlitSet, DepthUpsampleSet;
		struct CachedBindingSet
		{
			nvrhi::BindingSetHandle Set;
			uint64_t LastUsed = 0;
		};
		// Keyed by the five material textures (albedo, normal, metallic-roughness, occlusion, emissive).
		std::map<std::array<nvrhi::ITexture*, 5>, CachedBindingSet> MaterialSets;
		// Keyed by particle texture; they also reference the particle buffer, so they are rebuilt when it grows.
		std::map<nvrhi::ITexture*, CachedBindingSet> ParticleSets;

		// Per-frame scratch, kept to avoid reallocating every frame.
		std::vector<GpuDrawData> Draws;
		std::vector<GpuMaterialData> Materials;
		std::vector<GpuLightData> Lights;
		std::vector<DrawItem> Items;
		std::vector<TransparentDraw> Transparent;
		ParticleRenderData ParticleData;
		std::vector<ParticleDraw> ParticleDraws;
		// Particle problems are reported once per episode (while they persist), not every frame.
		bool ReportedParticleCap = false;
		bool ReportedParticleBufferFailure = false;
		bool ReportedParticleSetFailure = false;
		uint64_t FrameCounter = 0;
		bool FrameSetDirty = true;
		bool ReportedFrameSetFailure = false;
		bool ReportedDrawBufferFailure = false;
		bool ReportedShadowAtlasFailure = false;
		bool ReportedShadowOverflow = false;
		bool ReportedLightOverflow = false;
		bool ReportedInvalidSpotTransform = false;
	};

	SceneRenderer::SceneRenderer(nvrhi::IDevice* device, const SceneRendererSpecification& specification)
		: m_Impl(CreateScope<Impl>())
		, m_Device(device)
		, m_Resources(CreateScope<RenderResources>(device))
		, m_Profiler(device, std::vector<std::string>(std::begin(ScenePassNames), std::end(ScenePassNames)), specification.GpuTimers)
		, m_Picking(specification.Picking)
		, m_AsyncLoading(specification.AsyncLoading)
	{
		if (m_AsyncLoading)
		{
			// A placeholder must never wait on the pool: a cooperative wait could run an import here.
			m_PlaceholderMesh = Primitives::CreateCube();
			m_PlaceholderMesh->UpdateMemoryUsage();
		}
	}

	SceneRenderer::~SceneRenderer()
	{
		if (m_Device)
			m_Device->waitForIdle();
	}

	bool SceneRenderer::Initialize()
	{
		Impl& impl = *m_Impl;
		nvrhi::IDevice* device = m_Device;
		impl.CommandList = device->createCommandList();

		// --- Shaders ------------------------------------------------------------------------------
		impl.MeshVS = CreateEmbeddedShader(device, "Mesh.vert", nvrhi::ShaderType::Vertex);
		impl.DepthNormalPS = CreateEmbeddedShader(device, "DepthNormal.frag", nvrhi::ShaderType::Pixel);
		impl.PbrPS = CreateEmbeddedShader(device, "PBR.frag", nvrhi::ShaderType::Pixel);
		impl.ShadowVS = CreateEmbeddedShader(device, "Shadow.vert", nvrhi::ShaderType::Vertex);
		impl.ShadowMaskedVS = CreateEmbeddedShader(device, "ShadowMasked.vert", nvrhi::ShaderType::Vertex);
		impl.ShadowMaskedPS = CreateEmbeddedShader(device, "ShadowMasked.frag", nvrhi::ShaderType::Pixel);
		impl.FullscreenVS = CreateEmbeddedShader(device, "Fullscreen.vert", nvrhi::ShaderType::Vertex);
		impl.SsaoPS = CreateEmbeddedShader(device, "SSAO.frag", nvrhi::ShaderType::Pixel);
		impl.SsaoBlurPS = CreateEmbeddedShader(device, "SSAOBlur.frag", nvrhi::ShaderType::Pixel);
		impl.SkyboxVS = CreateEmbeddedShader(device, "Skybox.vert", nvrhi::ShaderType::Vertex);
		impl.SkyboxPS = CreateEmbeddedShader(device, "Skybox.frag", nvrhi::ShaderType::Pixel);
		impl.TonemapPS = CreateEmbeddedShader(device, "Tonemap.frag", nvrhi::ShaderType::Pixel);
		impl.BlitPS = CreateEmbeddedShader(device, "Blit.frag", nvrhi::ShaderType::Pixel);
		impl.DepthUpsamplePS = CreateEmbeddedShader(device, "DepthUpsample.frag", nvrhi::ShaderType::Pixel);
		impl.EquirectCS = CreateEmbeddedShader(device, "EquirectToCube.comp", nvrhi::ShaderType::Compute);
		impl.SkyCS = CreateEmbeddedShader(device, "ProceduralSky.comp", nvrhi::ShaderType::Compute);
		impl.IrradianceCS = CreateEmbeddedShader(device, "Irradiance.comp", nvrhi::ShaderType::Compute);
		impl.PrefilterCS = CreateEmbeddedShader(device, "Prefilter.comp", nvrhi::ShaderType::Compute);
		impl.BrdfCS = CreateEmbeddedShader(device, "BRDFLut.comp", nvrhi::ShaderType::Compute);
		impl.DownsampleCS = CreateEmbeddedShader(device, "CubeDownsample.comp", nvrhi::ShaderType::Compute);
		impl.ParticleVS = CreateEmbeddedShader(device, "Particle.vert", nvrhi::ShaderType::Vertex);
		impl.ParticlePS = CreateEmbeddedShader(device, "Particle.frag", nvrhi::ShaderType::Pixel);
		for (const nvrhi::ShaderHandle& shader : { impl.MeshVS, impl.DepthNormalPS, impl.PbrPS, impl.ShadowVS, impl.FullscreenVS, impl.SsaoPS, impl.SsaoBlurPS, impl.SkyboxVS, impl.SkyboxPS, impl.TonemapPS, impl.BlitPS, impl.DepthUpsamplePS, impl.EquirectCS, impl.SkyCS, impl.IrradianceCS, impl.PrefilterCS, impl.BrdfCS, impl.DownsampleCS, impl.ParticleVS, impl.ParticlePS })
		{
			if (!shader)
				return false;
		}

		if (m_Picking)
		{
			impl.DepthNormalPickPS = CreateEmbeddedShader(device, "DepthNormalPick.frag", nvrhi::ShaderType::Pixel);
			impl.PickPS = CreateEmbeddedShader(device, "Pick.frag", nvrhi::ShaderType::Pixel);
			impl.OutlinePS = CreateEmbeddedShader(device, "SelectionOutline.frag", nvrhi::ShaderType::Pixel);
			auto layout = MakeLayout(0, nvrhi::ShaderType::Pixel);
			layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
			layout.addItem(nvrhi::BindingLayoutItem::PushConstants(PushConstantSlot, sizeof(OutlinePush)));
			impl.OutlineLayout = device->createBindingLayout(layout);
			if (!impl.DepthNormalPickPS || !impl.PickPS || !impl.OutlinePS || !impl.OutlineLayout)
			{
				BS_CORE_ERROR("SceneRenderer: failed to initialize picking shaders/layout");
				return false;
			}
		}

		// --- Input layouts ------------------------------------------------------------------------
		const nvrhi::VertexAttributeDesc meshAttributes[] = {
			nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(offsetof(Vertex, Position)).setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(offsetof(Vertex, Normal)).setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc().setName("TANGENT").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(offsetof(Vertex, Tangent)).setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(Vertex, TexCoord)).setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc().setName("JOINTS").setFormat(nvrhi::Format::RGBA32_UINT).setOffset(offsetof(Vertex, Joints)).setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc().setName("WEIGHTS").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(offsetof(Vertex, Weights)).setElementStride(sizeof(Vertex)),
		};
		impl.MeshInputLayout = device->createInputLayout(meshAttributes, 6, impl.MeshVS);
		const nvrhi::VertexAttributeDesc shadowAttributes[] = { meshAttributes[0], meshAttributes[4], meshAttributes[5] };
		impl.ShadowInputLayout = device->createInputLayout(shadowAttributes, 3, impl.ShadowVS);
		const nvrhi::VertexAttributeDesc maskedShadowAttributes[] = { meshAttributes[0], meshAttributes[3], meshAttributes[4], meshAttributes[5] };
		impl.ShadowMaskedInputLayout = device->createInputLayout(maskedShadowAttributes, 4, impl.ShadowMaskedVS);

		// --- Binding layouts ----------------------------------------------------------------------
		// Light records and cluster lists share slot 3: keep this frame layout at four storage buffers,
		// the Vulkan core minimum per shader stage.
		nvrhi::BindingLayoutDesc frameDesc = MakeLayout(0, nvrhi::ShaderType::All);
		frameDesc.bindings = {
			nvrhi::BindingLayoutItem::PushConstants(PushConstantSlot, sizeof(DrawPush)),
			nvrhi::BindingLayoutItem::ConstantBuffer(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),
			nvrhi::BindingLayoutItem::Texture_SRV(4),
			nvrhi::BindingLayoutItem::Texture_SRV(5),
			nvrhi::BindingLayoutItem::Texture_SRV(6),
			nvrhi::BindingLayoutItem::Texture_SRV(7),
			nvrhi::BindingLayoutItem::Texture_SRV(8),
			nvrhi::BindingLayoutItem::Sampler(9),
			nvrhi::BindingLayoutItem::Sampler(10),
			nvrhi::BindingLayoutItem::Sampler(11),
			nvrhi::BindingLayoutItem::Texture_SRV(12),
			nvrhi::BindingLayoutItem::Texture_SRV(13),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(14),
		};
		impl.FrameLayout = device->createBindingLayout(frameDesc);
		// Shadow passes must not bind the depth textures they write as SRVs. In particular, switching
		// between opaque and masked pipelines can otherwise transition the active attachment to read-only.
		nvrhi::BindingLayoutDesc shadowFrameDesc = MakeLayout(0, nvrhi::ShaderType::AllGraphics);
		shadowFrameDesc.bindings = {
			nvrhi::BindingLayoutItem::PushConstants(PushConstantSlot, sizeof(DrawPush)),
			nvrhi::BindingLayoutItem::ConstantBuffer(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(14),
		};
		impl.ShadowFrameLayout = device->createBindingLayout(shadowFrameDesc);
		if (!impl.FrameLayout || !impl.ShadowFrameLayout)
		{
			BS_CORE_ERROR("SceneRenderer: failed to create {} frame layout", !impl.FrameLayout ? "lighting" : "shadow");
			return false;
		}

		nvrhi::BindingLayoutDesc materialDesc = MakeLayout(1, nvrhi::ShaderType::Pixel);
		materialDesc.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Texture_SRV(1),
			nvrhi::BindingLayoutItem::Texture_SRV(2),
			nvrhi::BindingLayoutItem::Texture_SRV(3),
			nvrhi::BindingLayoutItem::Texture_SRV(4),
			nvrhi::BindingLayoutItem::Sampler(5),
		};
		impl.MaterialLayout = device->createBindingLayout(materialDesc);

		nvrhi::BindingLayoutDesc ssaoDesc = MakeLayout(0, nvrhi::ShaderType::Pixel);
		ssaoDesc.bindings = {
			nvrhi::BindingLayoutItem::ConstantBuffer(0),
			nvrhi::BindingLayoutItem::Texture_SRV(1),
			nvrhi::BindingLayoutItem::Texture_SRV(2),
			nvrhi::BindingLayoutItem::Texture_SRV(3),
			nvrhi::BindingLayoutItem::Sampler(4),
			nvrhi::BindingLayoutItem::Sampler(5),
		};
		impl.SsaoLayout = device->createBindingLayout(ssaoDesc);

		nvrhi::BindingLayoutDesc blurDesc = MakeLayout(0, nvrhi::ShaderType::Pixel);
		blurDesc.bindings = {
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(BlurPush)),
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Texture_SRV(1),
			nvrhi::BindingLayoutItem::Sampler(2),
		};
		impl.BlurLayout = device->createBindingLayout(blurDesc);

		nvrhi::BindingLayoutDesc textureSamplerDesc = MakeLayout(0, nvrhi::ShaderType::Pixel);
		textureSamplerDesc.bindings = {
			nvrhi::BindingLayoutItem::PushConstants(0, TextureSamplerPushSize),
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(1),
		};
		impl.TextureSamplerLayout = device->createBindingLayout(textureSamplerDesc);

		nvrhi::BindingLayoutDesc tonemapDesc = MakeLayout(0, nvrhi::ShaderType::Pixel);
		tonemapDesc.bindings = {
			nvrhi::BindingLayoutItem::PushConstants(PushConstantSlot, sizeof(TonemapPush)),
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(1),
			nvrhi::BindingLayoutItem::Texture_SRV(2),
			nvrhi::BindingLayoutItem::Texture_SRV(3),
			nvrhi::BindingLayoutItem::Texture_SRV(4),
			nvrhi::BindingLayoutItem::Sampler(5),
		};
		impl.TonemapLayout = device->createBindingLayout(tonemapDesc);

		nvrhi::BindingLayoutDesc computeDesc = MakeLayout(0, nvrhi::ShaderType::Compute);
		computeDesc.bindings = {
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(SkyPush)),
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(1),
			nvrhi::BindingLayoutItem::Texture_UAV(2),
		};
		impl.ComputeLayout = device->createBindingLayout(computeDesc);

		// Set 1 of the particle pipelines (set 0 is FrameLayout): the vertex shader reads the particles, the
		// pixel shader the texture.
		nvrhi::BindingLayoutDesc particleDesc = MakeLayout(1, nvrhi::ShaderType::All);
		particleDesc.bindings = {
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(ParticleBufferSlot),
			nvrhi::BindingLayoutItem::Texture_SRV(ParticleTextureSlot),
			nvrhi::BindingLayoutItem::Sampler(ParticleSamplerSlot),
		};
		impl.ParticleLayout = device->createBindingLayout(particleDesc);

		// --- Samplers -----------------------------------------------------------------------------
		// Equirectangular maps wrap horizontally (longitude) and clamp at the poles.
		impl.EquirectSampler = device->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAddressU(nvrhi::SamplerAddressMode::Wrap).setAddressV(nvrhi::SamplerAddressMode::Clamp).setAddressW(nvrhi::SamplerAddressMode::Clamp));
		impl.LinearClamp = device->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
		impl.LinearWrapAniso = device->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap).setMaxAnisotropy(8.0f));
		impl.PointClamp = device->createSampler(nvrhi::SamplerDesc().setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
		impl.PointWrap = device->createSampler(nvrhi::SamplerDesc().setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap));
		impl.ShadowCompare = device->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setMipFilter(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp).setReductionType(nvrhi::SamplerReductionType::Comparison));

		// --- Buffers and constant textures --------------------------------------------------------
		nvrhi::BufferDesc constantDesc;
		constantDesc.byteSize = sizeof(GpuViewConstants);
		constantDesc.isConstantBuffer = true;
		constantDesc.debugName = "ViewConstants";
		constantDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
		constantDesc.keepInitialState = true;
		impl.ViewConstantsBuffer = device->createBuffer(constantDesc);
		impl.ViewMemory.Resize(impl.ViewConstantsBuffer ? static_cast<size_t>(constantDesc.byteSize) : 0);
		constantDesc.byteSize = sizeof(GpuSSAOConstants);
		constantDesc.debugName = "SSAOConstants";
		impl.SsaoConstantsBuffer = device->createBuffer(constantDesc);
		impl.SsaoMemory.Resize(impl.SsaoConstantsBuffer ? static_cast<size_t>(constantDesc.byteSize) : 0);

		impl.DrawBuffer = EnsureStructuredBuffer(device, nullptr, sizeof(GpuDrawData) * 256, sizeof(GpuDrawData), "DrawBuffer");
		impl.DrawMemory.Resize(impl.DrawBuffer ? static_cast<size_t>(impl.DrawBuffer->getDesc().byteSize) : 0);
		impl.MaterialBuffer = EnsureStructuredBuffer(device, nullptr, sizeof(GpuMaterialData) * 256, sizeof(GpuMaterialData), "MaterialBuffer");
		impl.MaterialMemory.Resize(impl.MaterialBuffer ? static_cast<size_t>(impl.MaterialBuffer->getDesc().byteSize) : 0);
		if (!impl.Clusters.Initialize(device))
			return false;
		impl.LightBuffer = impl.Clusters.GetBuffer();

		nvrhi::TextureDesc shadowDesc;
		shadowDesc.width = ShadowMapSize;
		shadowDesc.height = ShadowMapSize;
		shadowDesc.arraySize = MaxCascades;
		shadowDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
		shadowDesc.format = nvrhi::Format::D32;
		shadowDesc.isRenderTarget = true;
		shadowDesc.debugName = "ShadowMap";
		shadowDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		shadowDesc.keepInitialState = true;
		shadowDesc.setClearValue(nvrhi::Color(1.0f));
		impl.ShadowMap = device->createTexture(shadowDesc);
		for (uint32_t cascade = 0; cascade < MaxCascades; cascade++)
			impl.ShadowFramebuffers[cascade] = device->createFramebuffer(nvrhi::FramebufferDesc().setDepthAttachment(impl.ShadowMap, nvrhi::TextureSubresourceSet(0, 1, cascade, 1)));

		// Descriptors need a valid texture even when every local light opts out. Grow on first use.
		if (!CreateLocalShadowTarget(device, 1, impl.LocalShadowAtlas, impl.LocalShadowFramebuffer, true))
			return false;
		impl.LocalShadowBuffer = EnsureStructuredBuffer(device, nullptr, sizeof(LocalShadowMap) * MaxLocalShadowMaps, sizeof(LocalShadowMap), "LocalShadowBuffer");
		impl.LocalShadowMemory.Resize(impl.LocalShadowBuffer ? static_cast<size_t>(impl.LocalShadowBuffer->getDesc().byteSize) : 0);
		if (!impl.LocalShadowBuffer)
		{
			BS_CORE_ERROR("SceneRenderer: failed to create local shadow map buffer");
			return false;
		}

		// SSAO kernel: hemisphere samples concentrated near the origin; 4x4 rotation noise.
		// Basalt::Random is identical on every platform, and values are drawn in statement order (argument
		// evaluation order varies by compiler), so every platform builds the same kernel and image.
		Random random(1337u);
		auto unit = [&random]() { return random.Float(); };
		for (uint32_t i = 0; i < SSAOKernelSize; i++)
		{
			const float x = unit() * 2.0f - 1.0f;
			const float y = unit() * 2.0f - 1.0f;
			const float z = unit();
			glm::vec3 sample = glm::normalize(glm::vec3(x, y, z)) * unit();
			float scale = static_cast<float>(i) / static_cast<float>(SSAOKernelSize);
			scale = 0.1f + 0.9f * scale * scale;
			impl.SsaoConstants.Kernel[i] = glm::vec4(sample * scale, 0.0f);
		}
		std::array<uint8_t, size_t{ 16 } * 4> noise{};
		for (size_t i = 0; i < 16; i++)
		{
			noise[i * 4 + 0] = static_cast<uint8_t>(unit() * 255.0f);
			noise[i * 4 + 1] = static_cast<uint8_t>(unit() * 255.0f);
			noise[i * 4 + 3] = 255;
		}
		nvrhi::TextureDesc noiseDesc;
		noiseDesc.width = 4;
		noiseDesc.height = 4;
		noiseDesc.format = nvrhi::Format::RGBA8_UNORM;
		noiseDesc.debugName = "SSAONoise";
		noiseDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		noiseDesc.keepInitialState = true;
		impl.SsaoNoise = device->createTexture(noiseDesc);

		nvrhi::TextureDesc lutDesc;
		lutDesc.width = BRDFLutSize;
		lutDesc.height = BRDFLutSize;
		lutDesc.format = nvrhi::Format::RG16_FLOAT;
		lutDesc.isUAV = true;
		lutDesc.debugName = "BRDFLut";
		lutDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		lutDesc.keepInitialState = true;
		impl.BrdfLut = device->createTexture(lutDesc);

		const std::vector<uint8_t> softDisc = MakeSoftDiscTexture(ParticleTextureSize);
		nvrhi::TextureDesc particleTextureDesc;
		particleTextureDesc.width = ParticleTextureSize;
		particleTextureDesc.height = ParticleTextureSize;
		particleTextureDesc.format = nvrhi::Format::RGBA8_UNORM;
		particleTextureDesc.debugName = "ParticleSoftDisc";
		particleTextureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		particleTextureDesc.keepInitialState = true;
		impl.ParticleTexture = device->createTexture(particleTextureDesc);

		// --- Compute pipelines --------------------------------------------------------------------
		auto makeCompute = [&](nvrhi::IShader* shader) {
			nvrhi::ComputePipelineDesc desc;
			desc.CS = shader;
			desc.bindingLayouts = { impl.ComputeLayout };
			return device->createComputePipeline(desc);
		};
		if (!impl.Lines.Initialize(device))
			return false;
		impl.EquirectPipeline = makeCompute(impl.EquirectCS);
		impl.SkyPipeline = makeCompute(impl.SkyCS);
		impl.IrradiancePipeline = makeCompute(impl.IrradianceCS);
		impl.PrefilterPipeline = makeCompute(impl.PrefilterCS);
		impl.BrdfPipeline = makeCompute(impl.BrdfCS);
		impl.DownsamplePipeline = makeCompute(impl.DownsampleCS);

		// Opaque/masked pairs share the same depth policy. Only directional cascades use depth clamp.
		auto createShadowPipelines = [&](bool local, nvrhi::GraphicsPipelineHandle& opaque, nvrhi::GraphicsPipelineHandle& masked) {
			nvrhi::GraphicsPipelineDesc desc;
			desc.inputLayout = impl.ShadowInputLayout;
			desc.VS = impl.ShadowVS;
			desc.renderState.rasterState.setCullNone().setDepthBias(1).setDepthClipEnable(local);
			desc.renderState.rasterState.slopeScaledDepthBias = 1.5f;
			desc.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(true).setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
			desc.bindingLayouts = { impl.ShadowFrameLayout };
			const auto& framebufferInfo = impl.ShadowFramebuffers[0]->getFramebufferInfo();
			opaque = device->createGraphicsPipeline(desc, framebufferInfo);
			desc.inputLayout = impl.ShadowMaskedInputLayout;
			desc.VS = impl.ShadowMaskedVS;
			desc.PS = impl.ShadowMaskedPS;
			desc.bindingLayouts = { impl.ShadowFrameLayout, impl.MaterialLayout };
			masked = device->createGraphicsPipeline(desc, framebufferInfo);
			if (!opaque || !masked)
			{
				BS_CORE_ERROR("SceneRenderer: failed to create {} {} shadow pipeline", local ? "local" : "directional", !opaque ? "opaque" : "masked");
				return false;
			}
			return true;
		};
		if (!createShadowPipelines(false, impl.ShadowPipeline, impl.ShadowMaskedPipeline) ||
			!createShadowPipelines(true, impl.LocalShadowPipeline, impl.LocalShadowMaskedPipeline))
			return false;

		// Upload constant data.
		impl.CommandList->open();
		m_Resources->Initialize(impl.CommandList);
		impl.CommandList->writeTexture(impl.SsaoNoise, 0, 0, noise.data(), 16);
		impl.CommandList->writeTexture(impl.ParticleTexture, 0, 0, softDisc.data(), size_t{ ParticleTextureSize } * 4);
		impl.CommandList->close();
		device->executeCommandList(impl.CommandList);

		const bool valid = impl.ShadowFrameLayout && impl.LocalShadowPipeline && impl.LocalShadowMaskedPipeline && impl.MeshInputLayout && impl.ShadowInputLayout && impl.FrameLayout && impl.MaterialLayout && impl.SsaoLayout && impl.BlurLayout && impl.TextureSamplerLayout && impl.ComputeLayout && impl.ShadowMap && impl.ShadowPipeline && impl.ShadowMaskedPipeline && impl.ShadowMaskedInputLayout && impl.EquirectPipeline && impl.SkyPipeline && impl.IrradiancePipeline && impl.PrefilterPipeline && impl.BrdfPipeline && impl.DownsamplePipeline && impl.ViewConstantsBuffer && impl.ParticleLayout && impl.ParticleTexture;
		if (!valid)
			BS_CORE_ERROR("SceneRenderer: failed to create GPU resources");
		return valid;
	}

	void SceneRenderer::SetViewportSize(uint32_t width, uint32_t height)
	{
		ResizeTargets(width, height);
	}

	void SceneRenderer::SetRenderScale(float scale)
	{
		// Applied by the next SetViewportSize or Render, so setting size and scale in either order reallocates
		// the targets once.
		m_RenderScale = std::isfinite(scale) ? std::clamp(scale, MinRenderScale, MaxRenderScale) : 1.0f;
	}

	void SceneRenderer::SetDynamicResolution(const DynamicResolutionSettings& settings)
	{
		const DynamicResolutionSettings sanitized = SanitizeDynamicResolution(settings);
		if (sanitized == m_DynamicResolution)
			return;
		// Validated inputs never need this; a programmatic caller passing bad values should hear about it.
		if (!(sanitized == settings))
			BS_CORE_WARN("SceneRenderer: dynamic resolution settings out of range (target {} fps, minimum scale {}); using {} fps and {}", settings.TargetFrameRate, settings.MinScale, sanitized.TargetFrameRate, sanitized.MinScale);
		// Turning it on starts from the render scale. A new budget or minimum keeps the current scale (jumping
		// back to the highest scale would cost slow frames until samples arrive) but not the old costs.
		if (sanitized.Enabled != m_DynamicResolution.Enabled)
			m_DynamicController.Reset(m_RenderScale);
		else
			m_DynamicController.ForgetHistory();
		m_DynamicResolution = sanitized;
	}

	DynamicResolutionState SceneRenderer::GetDynamicResolutionState() const
	{
		return Basalt::GetDynamicResolutionState(m_DynamicResolution, m_RenderScale, HasGpuTimers());
	}

	bool SceneRenderer::IsDynamicResolutionActive() const
	{
		// An empty scale range would only cost the dynamic targets' resampling and overlay depth pass.
		return GetDynamicResolutionState() == DynamicResolutionState::Active;
	}

	float SceneRenderer::GetDynamicResolutionMilliseconds(const GpuFrameSample& sample)
	{
		// The environment bake runs once per sky change and uploads follow asset streaming; no resolution would
		// absorb either (a first frame full of uploads would otherwise drop to the minimum). Recurring
		// work, including the fixed cluster grid, must still fit within the requested frame budget.
		return sample.MillisecondsWithout({ PassEnvironment, PassUploads });
	}

	void SceneRenderer::ReportDynamicResolutionState()
	{
		// Once per change, at the start of a frame, when the settings and the render scale of the frame are both
		// set: hosts set them one after the other, and the state in between is not worth a warning.
		const DynamicResolutionState state = GetDynamicResolutionState();
		if (state == m_Diagnostics.ReportedState)
			return;
		m_Diagnostics.ReportedState = state;
		if (state == DynamicResolutionState::NoGpuTimers || state == DynamicResolutionState::EmptyRange)
			BS_CORE_WARN("SceneRenderer: dynamic resolution {}; rendering at the fixed render scale", DescribeDynamicResolutionState(state, m_DynamicResolution, m_RenderScale));
	}

	void SceneRenderer::UpdateRenderSize(const std::optional<GpuFrameSample>& sample)
	{
		ReportDynamicResolutionState();
		float scale = m_RenderScale;
		if (IsDynamicResolutionActive())
		{
			const float minScale = m_DynamicResolution.MinScale;
			if (sample)
			{
				const float milliseconds = GetDynamicResolutionMilliseconds(*sample);
				const float budget = 1000.0f / m_DynamicResolution.TargetFrameRate;
				// Against the current viewport: a result from before a resize then predicts the right cost.
				const float renderedScale = ComputePixelScale(sample->RenderedPixels, { m_Width, m_Height });
				if (m_DynamicController.AddSample(milliseconds, renderedScale, budget, minScale, m_RenderScale))
				{
					m_Diagnostics.Samples.Accepted++;
					m_Diagnostics.RejectedInARow = 0;
				}
				else
				{
					m_Diagnostics.Samples.Rejected++;
					// Timers that report 0 (coarse or saturating counters) would leave the scale frozen silently.
					if (++m_Diagnostics.RejectedInARow == GpuProfiler::AverageWindow && !m_Diagnostics.WarnedRejected)
					{
						BS_CORE_WARN("SceneRenderer: the last {} GPU timer results were unusable ({:.3f} ms); dynamic resolution cannot adjust", m_Diagnostics.RejectedInARow, milliseconds);
						m_Diagnostics.WarnedRejected = true;
					}
				}
			}
			scale = m_DynamicController.GetScale(minScale, m_RenderScale);
		}
		else
		{
			// Inactive (also when SetRenderScale emptied the range): start from the top when it becomes active.
			m_DynamicController.Reset(m_RenderScale);
		}

		const glm::uvec2 size = ComputeDynamicRenderSize(m_Width, m_Height, scale, { m_TargetWidth, m_TargetHeight });
		m_RenderWidth = size.x;
		m_RenderHeight = size.y;
		m_CurrentRenderScale = scale;
		// ResizeTargets ran first in this frame with the same settings; only dynamic targets may be rendered
		// partially (the others' samplers and overlay depth assume the render area fills them).
		BS_CORE_ASSERT(m_TargetsDynamic == IsDynamicResolutionActive() && (m_TargetsDynamic || (m_RenderWidth == m_TargetWidth && m_RenderHeight == m_TargetHeight)), "the render area does not match the targets' configuration");
	}

	void SceneRenderer::ResizeTargets(uint32_t width, uint32_t height)
	{
		width = std::max(width, 1u);
		height = std::max(height, 1u);
		const glm::uvec2 targetSize = ComputeRenderSize(width, height, m_RenderScale);
		const bool dynamic = IsDynamicResolutionActive();
		if (width == m_Width && height == m_Height && targetSize.x == m_TargetWidth && targetSize.y == m_TargetHeight && dynamic == m_TargetsDynamic && m_OutputTexture)
			return;

		Impl& impl = *m_Impl;
		m_Device->waitForIdle();
		InvalidatePicking();
		impl.Upscaler.Reset();
		impl.FsrTonemapPipeline = nullptr;
		impl.FxaaTonemapPipeline = nullptr;
		impl.FxaaTonemapFailed = false;
		impl.TonemapSetFailed = false;
		impl.TonemapSource = nullptr;
		impl.PostProcess.Reset();
		impl.FsrTonemapFailed = false;
		// The smoothed costs are per frame of the old viewport size; results still in flight carry their pixel
		// count, so they are converted correctly when they arrive.
		if (width != m_Width || height != m_Height)
			m_DynamicController.ForgetHistory();
		m_Width = width;
		m_Height = height;
		m_TargetWidth = targetSize.x;
		m_TargetHeight = targetSize.y;
		m_TargetsDynamic = dynamic;
		m_RenderWidth = m_TargetWidth;
		m_RenderHeight = m_TargetHeight;
		m_CurrentRenderScale = m_RenderScale;
		// Dynamic resolution can render below the target size at any time, so it always resamples.
		const bool scaled = dynamic || m_TargetWidth != m_Width || m_TargetHeight != m_Height;

		// Scene targets have the target size; the output and the overlay depth have the viewport size.
		auto makeTarget = [&](nvrhi::Format format, const char* name, bool isDepth = false, bool outputSize = false) {
			nvrhi::TextureDesc desc;
			desc.width = outputSize ? m_Width : m_TargetWidth;
			desc.height = outputSize ? m_Height : m_TargetHeight;
			desc.format = format;
			desc.isRenderTarget = true;
			desc.debugName = name;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.setClearValue(isDepth ? nvrhi::Color(1.0f) : nvrhi::Color(0.0f));
			return m_Device->createTexture(desc);
		};

		m_DepthTexture = makeTarget(nvrhi::Format::D32, "SceneDepth", true);
		impl.NormalTexture = makeTarget(nvrhi::Format::RGBA16_FLOAT, "SceneNormals");
		impl.HdrTexture = makeTarget(nvrhi::Format::RGBA16_FLOAT, "SceneHDR");
		impl.SsaoTexture = makeTarget(nvrhi::Format::R8_UNORM, "SSAO");
		impl.SsaoBlurTexture = makeTarget(nvrhi::Format::R8_UNORM, "SSAOBlur");
		m_OutputTexture = makeTarget(nvrhi::Format::RGBA8_UNORM, "SceneOutput", false, true);
		// Debug lines are drawn over the output and depth-tested against the scene, so a scaled scene's depth
		// is resampled to the output size (see the depth upsample pass in Render).
		impl.OverlayDepth = scaled ? makeTarget(nvrhi::Format::D32, "OverlayDepth", true, true) : nullptr;

		nvrhi::FramebufferDesc prepassDesc;
		prepassDesc.addColorAttachment(impl.NormalTexture).setDepthAttachment(m_DepthTexture);
		if (m_Picking)
		{
			impl.EntityIds = makeTarget(nvrhi::Format::R32_UINT, "EntityIds");
			impl.PickDepth = makeTarget(nvrhi::Format::D32, "PickDepth", true);
			if (!impl.EntityIds || !impl.PickDepth)
			{
				BS_CORE_ERROR("SceneRenderer: failed to allocate picking targets");
				m_OutputTexture = nullptr;
				return;
			}
			prepassDesc.addColorAttachment(impl.EntityIds);
			impl.PickFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(impl.EntityIds).setDepthAttachment(impl.PickDepth));
			impl.OutlineSet = m_Device->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::Texture_SRV(0, impl.EntityIds)).addItem(nvrhi::BindingSetItem::PushConstants(PushConstantSlot, sizeof(OutlinePush))), impl.OutlineLayout);
		}
		impl.PrepassFramebuffer = m_Device->createFramebuffer(prepassDesc);
		if (m_Picking && (!impl.PrepassFramebuffer || !impl.PickFramebuffer || !impl.OutlineSet))
		{
			BS_CORE_ERROR("SceneRenderer: failed to create picking framebuffer/bindings");
			m_OutputTexture = nullptr;
			return;
		}
		impl.LightingFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(impl.HdrTexture).setDepthAttachment(m_DepthTexture));
		impl.SsaoFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(impl.SsaoTexture));
		impl.SsaoBlurFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(impl.SsaoBlurTexture));
		impl.OutputFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_OutputTexture));
		m_OverlayFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_OutputTexture).setDepthAttachment(scaled ? impl.OverlayDepth : m_DepthTexture));
		impl.OverlayDepthFramebuffer = scaled ? m_Device->createFramebuffer(nvrhi::FramebufferDesc().setDepthAttachment(impl.OverlayDepth)) : nullptr;

		// Pipelines depend only on formats, but are cheap to create; rebuild them with the targets.
		auto meshPipeline = [&](nvrhi::IShader* pixelShader, nvrhi::IFramebuffer* framebuffer, bool doubleSided, bool depthWrite, bool blend) {
			nvrhi::GraphicsPipelineDesc desc;
			desc.inputLayout = impl.MeshInputLayout;
			desc.VS = impl.MeshVS;
			desc.PS = pixelShader;
			// Meshes wind counter-clockwise (glTF convention).
			desc.renderState.rasterState.setFrontCounterClockwise(true);
			if (doubleSided)
				desc.renderState.rasterState.setCullNone();
			else
				desc.renderState.rasterState.setCullBack();
			desc.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(depthWrite).setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
			if (blend)
			{
				desc.renderState.blendState.targets[0]
					.setBlendEnable(true)
					.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
					.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
					.setSrcBlendAlpha(nvrhi::BlendFactor::One)
					.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
			}
			desc.bindingLayouts = { impl.FrameLayout, impl.MaterialLayout };
			return m_Device->createGraphicsPipeline(desc, framebuffer->getFramebufferInfo());
		};
		for (int doubleSided = 0; doubleSided < 2; doubleSided++)
		{
			impl.PrepassPipeline[doubleSided] = meshPipeline(m_Picking ? impl.DepthNormalPickPS : impl.DepthNormalPS, impl.PrepassFramebuffer, doubleSided, true, false);
			if (m_Picking && impl.PickFramebuffer)
				impl.PickPipeline[doubleSided] = meshPipeline(impl.PickPS, impl.PickFramebuffer, doubleSided, true, false);
			impl.OpaquePipeline[doubleSided] = meshPipeline(impl.PbrPS, impl.LightingFramebuffer, doubleSided, false, false);
			impl.TransparentPipeline[doubleSided] = meshPipeline(impl.PbrPS, impl.LightingFramebuffer, doubleSided, false, true);
		}

		if (m_Picking)
		{
			impl.OutlinePipeline = CreateFullscreenPipeline(m_Device, impl.FullscreenVS, impl.OutlinePS, impl.OutlineLayout, impl.OutputFramebuffer);
			if (!impl.PickFramebuffer || !impl.OutlineSet || !impl.OutlinePipeline || !impl.PickPipeline[0] || !impl.PickPipeline[1] || !impl.PrepassPipeline[0] || !impl.PrepassPipeline[1])
			{
				BS_CORE_ERROR("SceneRenderer: failed to create picking pipelines/bindings");
				m_OutputTexture = nullptr;
				return;
			}
		}

		// Billboards: depth-tested against the scene but not written (they are blended); no face culling.
		for (size_t blendIndex = 0; blendIndex < ParticleBlendModeCount; blendIndex++)
		{
			const auto blend = static_cast<ParticleBlendMode>(blendIndex);
			nvrhi::GraphicsPipelineDesc desc;
			desc.VS = impl.ParticleVS;
			desc.PS = impl.ParticlePS;
			desc.renderState.rasterState.setCullNone();
			desc.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(false).setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
			// Alpha blends like transparent meshes; additive adds color weighted by opacity and keeps the
			// target's alpha.
			const bool additive = blend == ParticleBlendMode::Additive;
			desc.renderState.blendState.targets[0]
				.setBlendEnable(true)
				.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
				.setDestBlend(additive ? nvrhi::BlendFactor::One : nvrhi::BlendFactor::InvSrcAlpha)
				.setSrcBlendAlpha(additive ? nvrhi::BlendFactor::Zero : nvrhi::BlendFactor::One)
				.setDestBlendAlpha(additive ? nvrhi::BlendFactor::One : nvrhi::BlendFactor::InvSrcAlpha);
			desc.bindingLayouts = { impl.FrameLayout, impl.ParticleLayout };
			impl.ParticlePipeline[blendIndex] = m_Device->createGraphicsPipeline(desc, impl.LightingFramebuffer->getFramebufferInfo());
		}

		impl.SsaoPipeline = CreateFullscreenPipeline(m_Device, impl.FullscreenVS, impl.SsaoPS, impl.SsaoLayout, impl.SsaoFramebuffer, false);
		impl.SsaoBlurPipeline = CreateFullscreenPipeline(m_Device, impl.FullscreenVS, impl.SsaoBlurPS, impl.BlurLayout, impl.SsaoBlurFramebuffer, false);
		impl.TonemapPipeline = CreateFullscreenPipeline(m_Device, impl.FullscreenVS, impl.TonemapPS, impl.TonemapLayout, impl.OutputFramebuffer, false);
		impl.SkyboxPipeline = CreateFullscreenPipeline(m_Device, impl.SkyboxVS, impl.SkyboxPS, impl.FrameLayout, impl.LightingFramebuffer, true);
		impl.DepthUpsamplePipeline = nullptr;
		if (scaled)
		{
			nvrhi::GraphicsPipelineDesc desc;
			desc.VS = impl.FullscreenVS;
			desc.PS = impl.DepthUpsamplePS;
			desc.renderState.rasterState.setCullNone();
			desc.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(true).setDepthFunc(nvrhi::ComparisonFunc::Always);
			desc.bindingLayouts = { impl.TextureSamplerLayout };
			impl.DepthUpsamplePipeline = m_Device->createGraphicsPipeline(desc, impl.OverlayDepthFramebuffer->getFramebufferInfo());
		}

		impl.SsaoSet = m_Device->createBindingSet(nvrhi::BindingSetDesc().setTrackLiveness(true).addItem(nvrhi::BindingSetItem::ConstantBuffer(0, impl.SsaoConstantsBuffer)).addItem(nvrhi::BindingSetItem::Texture_SRV(1, m_DepthTexture)).addItem(nvrhi::BindingSetItem::Texture_SRV(2, impl.NormalTexture)).addItem(nvrhi::BindingSetItem::Texture_SRV(3, impl.SsaoNoise)).addItem(nvrhi::BindingSetItem::Sampler(4, impl.PointClamp)).addItem(nvrhi::BindingSetItem::Sampler(5, impl.PointWrap)), impl.SsaoLayout);
		impl.BlurHorizontalSet = m_Device->createBindingSet(nvrhi::BindingSetDesc()
																.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(BlurPush)))
																.addItem(nvrhi::BindingSetItem::Texture_SRV(0, impl.SsaoTexture))
																.addItem(nvrhi::BindingSetItem::Texture_SRV(1, m_DepthTexture))
																.addItem(nvrhi::BindingSetItem::Sampler(2, impl.PointClamp)),
															impl.BlurLayout);
		impl.BlurVerticalSet = m_Device->createBindingSet(nvrhi::BindingSetDesc()
															  .addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(BlurPush)))
															  .addItem(nvrhi::BindingSetItem::Texture_SRV(0, impl.SsaoBlurTexture))
															  .addItem(nvrhi::BindingSetItem::Texture_SRV(1, m_DepthTexture))
															  .addItem(nvrhi::BindingSetItem::Sampler(2, impl.PointClamp)),
														  impl.BlurLayout);
		impl.TonemapSet = impl.CreateTonemapSet(m_Device, impl.HdrTexture, m_DepthTexture);
		if (impl.TonemapSet)
			impl.TonemapSource = impl.HdrTexture;
		else
		{
			BS_CORE_ERROR("SceneRenderer: failed to create initial tonemap bindings");
			impl.TonemapSetFailed = true;
			// Keep the source unset so Render's failed-set gate aborts before any tonemap draw.
			impl.TonemapSource = nullptr;
		}
		impl.DepthUpsampleSet = scaled ? m_Device->createBindingSet(nvrhi::BindingSetDesc()
																		.addItem(nvrhi::BindingSetItem::PushConstants(0, TextureSamplerPushSize))
																		.addItem(nvrhi::BindingSetItem::Texture_SRV(0, m_DepthTexture))
																		.addItem(nvrhi::BindingSetItem::Sampler(1, impl.PointClamp)),
																	impl.TextureSamplerLayout)
									   : nullptr;
		impl.BlitSet = m_Device->createBindingSet(nvrhi::BindingSetDesc()
													  .addItem(nvrhi::BindingSetItem::PushConstants(0, TextureSamplerPushSize))
													  .addItem(nvrhi::BindingSetItem::Texture_SRV(0, m_OutputTexture))
													  .addItem(nvrhi::BindingSetItem::Sampler(1, impl.LinearClamp)),
												  impl.TextureSamplerLayout);
		impl.BlitPipelines.clear();
		impl.FrameSetDirty = true;
	}

	void SceneRenderer::ResetExposure()
	{
		m_Impl->ExposureRevision = 0;
	}

	void SceneRenderer::Render(Scene& scene, const RenderCamera& camera, float deltaTime)
	{
		BS_PROFILE_SCOPE("Scene render");
		// Per-frame fence publishes worker results before any scene resource lookup.
		AssetManager::PublishCompleted();
		InvalidatePicking();
		Impl& impl = *m_Impl;
		ResizeTargets(m_Width, m_Height);
		if (!m_OutputTexture)
		{
			m_Statistics.Failed = true;
			return;
		}

		nvrhi::ICommandList* commandList = impl.CommandList;
		commandList->open();
		GpuProfiler& profiler = m_Profiler;
		UpdateRenderSize(profiler.BeginFrame(commandList));
		impl.FrameCounter++;
		m_Statistics = {};
		RendererSettings settings = scene.GetRendererSettings();
		UUID postProcessId = 0;
		const PostProcessComponent post = ResolvePostProcess(scene, &postProcessId);
		CopyPostProcessSettings(post, settings);
		const auto graphics = scene.GetEffectiveGraphicsSettings();
		settings.SSAOEnabled = settings.SSAOEnabled && graphics.SSAOQuality != GraphicsQuality::Off;
		settings.ShadowsEnabled = settings.ShadowsEnabled && graphics.ShadowQuality != GraphicsQuality::Off;
		const bool orthographic = IsOrthographic(camera.Projection);
		auto abortFrame = [&]() {
			m_Statistics.Failed = true;
			profiler.EndFrame(commandList, 0);
			commandList->close();
			m_Device->executeCommandList(commandList);
		};

		if (const auto error = ValidatePostProcess(post); postProcessId != 0 && !error.empty())
		{
			if (impl.InvalidPostProcessError != error || impl.InvalidPostProcessEntity != postProcessId)
				BS_CORE_ERROR("SceneRenderer: invalid PostProcess on entity {}: {}", static_cast<uint64_t>(postProcessId), error);
			impl.InvalidPostProcessError = error;
			impl.InvalidPostProcessEntity = postProcessId;
			abortFrame();
			return;
		}

		impl.InvalidPostProcessError.clear();

		// --- Lights -------------------------------------------------------------------------------
		// The primary directional light (with shadow cascades) goes into the view constants; the other
		// directional lights are unshadowed and lead the light buffer, so MaxLights drops local lights first.
		const std::vector<DirectionalLightInfo> directionalLights = CollectDirectionalLights(scene);
		GpuDirectionalLight sun{};
		sun.Direction = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
		if (!directionalLights.empty())
		{
			const DirectionalLightInfo& primary = directionalLights.front();
			sun.Direction = glm::vec4(primary.Direction, 1.0f);
			sun.Color = glm::vec4(primary.Radiance, (primary.CastShadows && settings.ShadowsEnabled) ? 1.0f : 0.0f);
			sun.ShadowParams = glm::vec4(settings.ShadowBias, settings.ShadowNormalBias, primary.ShadowSoftness, static_cast<float>(MaxCascades));
			m_Statistics.Lights++;
		}

		std::vector<GpuLightData>& lights = impl.Lights;
		lights.clear();
		std::vector<LocalShadowRequest> shadowRequests;
		bool invalidSpotTransform = false;
		auto requestShadow = [&](const auto& light, LocalShadowRequest request) {
			if (settings.ShadowsEnabled && light.CastShadows && light.Intensity > 0.0f)
			{
				request.LightIndex = static_cast<uint32_t>(lights.size());
				shadowRequests.push_back(request);
			}
		};
		{
			for (size_t i = 1; i < directionalLights.size(); i++)
			{
				if (lights.size() >= MaxLights)
				{
					m_Statistics.LightsSkipped++;
					continue;
				}
				const DirectionalLightInfo& light = directionalLights[i];
				lights.push_back({ glm::vec4(0.0f), glm::vec4(light.Radiance, static_cast<float>(GpuLightType::Directional)), glm::vec4(light.Direction, 0.0f), glm::vec4(0.0f) });
			}
			auto pointView = scene.GetAllEntitiesWith<PointLightComponent>();
			for (entt::entity handle : pointView)
			{
				if (lights.size() >= MaxLights)
				{
					m_Statistics.LightsSkipped++;
					continue;
				}
				const auto& light = pointView.get<PointLightComponent>(handle);
				const glm::vec3 position(scene.GetWorldTransform({ handle, &scene })[3]);
				requestShadow(light, { Entity(handle, &scene).GetUUID(), 0, LocalShadowType::Point, position, light.Radius });
				const float softness = SanitizeLocalShadowSoftness(light.ShadowSoftness);
				lights.push_back({ glm::vec4(position, light.Radius), glm::vec4(light.Color * light.Intensity, static_cast<float>(GpuLightType::Point)), glm::vec4(0.0f), glm::vec4(0.0f, 0.0f, softness, 0.0f) });
			}
			auto spotView = scene.GetAllEntitiesWith<SpotLightComponent>();
			for (entt::entity handle : spotView)
			{
				if (lights.size() >= MaxLights)
				{
					m_Statistics.LightsSkipped++;
					continue;
				}
				const auto& light = spotView.get<SpotLightComponent>(handle);
				const glm::mat4 world = scene.GetWorldTransform({ handle, &scene });
				const glm::vec3 forward(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
				const float length = glm::length(forward);
				if (!std::isfinite(length) || length < 1e-6f)
				{
					invalidSpotTransform |= !std::isfinite(length);
					continue;
				}
				const glm::vec3 direction = forward / length;
				const float outerDegrees = std::clamp(light.OuterConeAngle, 0.1f, 89.9f);
				const float outer = glm::cos(glm::radians(outerDegrees));
				const float inner = glm::cos(glm::radians(std::clamp(light.InnerConeAngle, 0.0f, outerDegrees)));
				requestShadow(light, { Entity(handle, &scene).GetUUID(), 0, LocalShadowType::Spot, glm::vec3(world[3]), light.Range, direction, outerDegrees });
				const float softness = SanitizeLocalShadowSoftness(light.ShadowSoftness);
				lights.push_back({ glm::vec4(glm::vec3(world[3]), light.Range), glm::vec4(light.Color * light.Intensity, static_cast<float>(GpuLightType::Spot)), glm::vec4(direction, outer), glm::vec4(std::max(inner, outer + 1e-4f), 0.0f, softness, 0.0f) });
			}
		}
		const uint32_t bufferedLightCount = static_cast<uint32_t>(lights.size());
		m_Statistics.Lights += bufferedLightCount;
		if (m_Statistics.LightsSkipped && !impl.ReportedLightOverflow)
			BS_CORE_WARN("SceneRenderer: light budget {} exceeded; {} lights skipped", MaxLights, m_Statistics.LightsSkipped);
		impl.ReportedLightOverflow = m_Statistics.LightsSkipped > 0;
		if (invalidSpotTransform && !impl.ReportedInvalidSpotTransform)
			BS_CORE_WARN("SceneRenderer: skipped spot lights with non-finite world directions");
		impl.ReportedInvalidSpotTransform = invalidSpotTransform;
		const LocalShadowPlan localShadows = BuildLocalShadowPlan(shadowRequests, camera.View, camera.Projection);
		m_Statistics.LocalShadowMaps = static_cast<uint32_t>(localShadows.Maps.size());
		m_Statistics.LocalShadowsSkipped = localShadows.Skipped;
		if (localShadows.Skipped && !impl.ReportedShadowOverflow)
			BS_CORE_WARN("SceneRenderer: local shadow budget {} exceeded; {} visible lights remain unshadowed", MaxLocalShadowMaps, localShadows.Skipped);
		impl.ReportedShadowOverflow = localShadows.Skipped > 0;
		if (!localShadows.Maps.empty() && impl.LocalShadowAtlas->getDesc().width == 1)
		{
			if (!CreateLocalShadowTarget(m_Device, LocalShadowAtlasSize, impl.LocalShadowAtlas, impl.LocalShadowFramebuffer, !impl.ReportedShadowAtlasFailure))
			{
				impl.ReportedShadowAtlasFailure = true;
				abortFrame();
				return;
			}
			impl.ReportedShadowAtlasFailure = false;
			impl.FrameSetDirty = true;
		}
		m_Statistics.LocalShadowAtlasSize = impl.LocalShadowAtlas->getDesc().width;
		for (const auto& allocation : localShadows.Allocations)
		{
			BS_CORE_ASSERT(allocation.LightIndex < lights.size(), "shadow allocations index the light buffer");
			lights[allocation.LightIndex].Params.y = static_cast<float>(allocation.FirstMap + 1);
		}

		// --- Environment --------------------------------------------------------------------------
		const SkyLightComponent* skyLight = nullptr;
		{
			auto view = scene.GetAllEntitiesWith<SkyLightComponent>();
			if (auto it = view.begin(); it != view.end())
				skyLight = &view.get<SkyLightComponent>(*it);
		}
		const std::string environmentKey = skyLight ? skyLight->EnvironmentMap : std::string();
		Ref<TextureSource> environmentSource = environmentKey.empty() ? nullptr : (m_AsyncLoading ? AssetManager::RequestTexture(environmentKey) : AssetManager::GetTexture(environmentKey));
		std::string bakeKey = environmentKey;
		if (environmentSource)
		{
			bakeKey += "|" + std::to_string(reinterpret_cast<uintptr_t>(environmentSource.get()));
		}
		else
		{
			// The procedural sky depends on the sun direction; rebake when it moves noticeably.
			const glm::ivec3 quantized = glm::ivec3(glm::round(glm::vec3(sun.Direction) * 64.0f));
			bakeKey += "|sky|" + std::to_string(quantized.x) + "," + std::to_string(quantized.y) + "," + std::to_string(quantized.z);
		}
		const bool bakeEnvironment = bakeKey != impl.EnvironmentKey || !impl.Env.Cube;
		std::optional<GpuPassScope> environmentPass;
		if (!impl.BrdfLutReady || bakeEnvironment)
			environmentPass.emplace(profiler, commandList, PassEnvironment);

		// --- BRDF LUT (once) ----------------------------------------------------------------------
		if (!impl.BrdfLutReady)
		{
			nvrhi::BindingSetHandle set = m_Device->createBindingSet(nvrhi::BindingSetDesc()
																		 .addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(SkyPush)))
																		 .addItem(nvrhi::BindingSetItem::Texture_SRV(0, m_Resources->GetWhiteTexture()))
																		 .addItem(nvrhi::BindingSetItem::Sampler(1, impl.LinearClamp))
																		 .addItem(nvrhi::BindingSetItem::Texture_UAV(2, impl.BrdfLut)),
																	 impl.ComputeLayout);
			commandList->setComputeState(nvrhi::ComputeState().setPipeline(impl.BrdfPipeline).addBindingSet(set));
			SkyPush unused{};
			commandList->setPushConstants(&unused, sizeof(unused));
			commandList->dispatch((BRDFLutSize + 7) / 8, (BRDFLutSize + 7) / 8, 1);
			impl.BrdfLutReady = true;
		}

		if (bakeEnvironment)
		{
			impl.EnvironmentKey = bakeKey;
			impl.FrameSetDirty = true;

			auto makeCube = [&](uint32_t size, uint32_t mips, const char* name) {
				nvrhi::TextureDesc desc;
				desc.width = size;
				desc.height = size;
				desc.arraySize = 6;
				desc.mipLevels = mips;
				desc.dimension = nvrhi::TextureDimension::TextureCube;
				desc.format = nvrhi::Format::RGBA16_FLOAT;
				desc.isUAV = true;
				desc.debugName = name;
				desc.initialState = nvrhi::ResourceStates::ShaderResource;
				desc.keepInitialState = true;
				return m_Device->createTexture(desc);
			};
			const uint32_t environmentMips = static_cast<uint32_t>(std::log2(EnvironmentSize)) + 1;
			impl.Env.Cube = makeCube(EnvironmentSize, environmentMips, "EnvironmentCube");
			impl.Env.Irradiance = makeCube(IrradianceSize, 1, "IrradianceCube");
			impl.Env.Prefiltered = makeCube(PrefilteredSize, PrefilteredMips, "PrefilteredCube");

			auto computeSet = [&](nvrhi::ITexture* input, nvrhi::TextureDimension inputDimension, nvrhi::TextureSubresourceSet inputSubresources, nvrhi::ITexture* output, uint32_t outputMip, nvrhi::TextureDimension outputDimension, nvrhi::ISampler* sampler) {
				return m_Device->createBindingSet(nvrhi::BindingSetDesc()
													  .addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(SkyPush)))
													  .addItem(nvrhi::BindingSetItem::Texture_SRV(0, input, nvrhi::Format::UNKNOWN, inputSubresources, inputDimension))
													  .addItem(nvrhi::BindingSetItem::Sampler(1, sampler))
													  .addItem(nvrhi::BindingSetItem::Texture_UAV(2, output, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(outputMip, 1, 0, 6), outputDimension)),
												  impl.ComputeLayout);
			};
			const uint32_t groups = (EnvironmentSize + 7) / 8;

			// 1. Base environment cube: from the HDRI, or the procedural sky.
			nvrhi::ITexture* equirect = nullptr;
			if (!environmentKey.empty())
			{
				equirect = environmentSource ? m_Resources->GetTexture(environmentSource, false, commandList) : nullptr;
				if (environmentSource && environmentSource->Format != TextureFormat::RGBA32F)
					BS_CORE_WARN("SceneRenderer: environment '{}' is not an HDR image; lighting will be dim", environmentKey);
			}
			if (equirect)
			{
				nvrhi::BindingSetHandle set = computeSet(equirect, nvrhi::TextureDimension::Texture2D, nvrhi::AllSubresources, impl.Env.Cube, 0, nvrhi::TextureDimension::Texture2DArray, impl.EquirectSampler);
				commandList->setComputeState(nvrhi::ComputeState().setPipeline(impl.EquirectPipeline).addBindingSet(set));
				SkyPush unused{};
				commandList->setPushConstants(&unused, sizeof(unused));
				commandList->dispatch(groups, groups, 6);
			}
			else
			{
				SkyPush sky;
				sky.ZenithColor = glm::vec4(0.18f, 0.32f, 0.62f, 1.0f);
				sky.HorizonColor = glm::vec4(0.70f, 0.78f, 0.88f, 1.0f);
				sky.GroundColor = glm::vec4(0.16f, 0.15f, 0.14f, 1.0f);
				sky.SunDirection = glm::vec4(-glm::vec3(sun.Direction), 8.0f);
				nvrhi::BindingSetHandle set = computeSet(m_Resources->GetWhiteTexture(), nvrhi::TextureDimension::Texture2D, nvrhi::AllSubresources, impl.Env.Cube, 0, nvrhi::TextureDimension::Texture2DArray, impl.LinearClamp);
				commandList->setComputeState(nvrhi::ComputeState().setPipeline(impl.SkyPipeline).addBindingSet(set));
				commandList->setPushConstants(&sky, sizeof(sky));
				commandList->dispatch(groups, groups, 6);
			}

			// 2. Mip chain of the base cube (sampled by irradiance and prefilter passes).
			for (uint32_t mip = 1; mip < environmentMips; mip++)
			{
				const uint32_t size = std::max(1u, EnvironmentSize >> mip);
				nvrhi::BindingSetHandle set = computeSet(impl.Env.Cube, nvrhi::TextureDimension::Texture2DArray, nvrhi::TextureSubresourceSet(mip - 1, 1, 0, 6), impl.Env.Cube, mip, nvrhi::TextureDimension::Texture2DArray, impl.LinearClamp);
				commandList->setComputeState(nvrhi::ComputeState().setPipeline(impl.DownsamplePipeline).addBindingSet(set));
				SkyPush unused{};
				commandList->setPushConstants(&unused, sizeof(unused));
				commandList->dispatch((size + 7) / 8, (size + 7) / 8, 6);
			}

			// 3. Diffuse irradiance.
			{
				nvrhi::BindingSetHandle set = computeSet(impl.Env.Cube, nvrhi::TextureDimension::TextureCube, nvrhi::AllSubresources, impl.Env.Irradiance, 0, nvrhi::TextureDimension::Texture2DArray, impl.LinearClamp);
				commandList->setComputeState(nvrhi::ComputeState().setPipeline(impl.IrradiancePipeline).addBindingSet(set));
				SkyPush unused{};
				commandList->setPushConstants(&unused, sizeof(unused));
				commandList->dispatch((IrradianceSize + 7) / 8, (IrradianceSize + 7) / 8, 6);
			}

			// 4. Specular prefiltering, one roughness level per mip.
			for (uint32_t mip = 0; mip < PrefilteredMips; mip++)
			{
				const uint32_t size = std::max(1u, PrefilteredSize >> mip);
				nvrhi::BindingSetHandle set = computeSet(impl.Env.Cube, nvrhi::TextureDimension::TextureCube, nvrhi::AllSubresources, impl.Env.Prefiltered, mip, nvrhi::TextureDimension::Texture2DArray, impl.LinearClamp);
				commandList->setComputeState(nvrhi::ComputeState().setPipeline(impl.PrefilterPipeline).addBindingSet(set));
				struct
				{
					float Roughness;
					float EnvironmentResolution;
					float Padding[14];
				} push = { static_cast<float>(mip) / static_cast<float>(PrefilteredMips - 1), static_cast<float>(EnvironmentSize), {} };
				commandList->setPushConstants(&push, sizeof(SkyPush));
				commandList->dispatch((size + 7) / 8, (size + 7) / 8, 6);
			}
		}
		environmentPass.reset();

		// --- Camera, cascades and view constants --------------------------------------------------
		GpuViewConstants view{};
		view.View = camera.View;
		view.Projection = camera.Projection;
		view.ViewProjection = camera.Projection * camera.View;
		view.InverseView = glm::inverse(camera.View);
		view.InverseProjection = glm::inverse(camera.Projection);
		view.CameraPosition = glm::vec4(camera.Position, 1.0f);
		view.ViewportSize = glm::vec4(static_cast<float>(m_RenderWidth), static_cast<float>(m_RenderHeight), 1.0f / static_cast<float>(m_TargetWidth), 1.0f / static_cast<float>(m_TargetHeight));
		// Local biases stay available without a sun. They share scene controls with the sun, but
		// ShadowBias is range-relative world distance here, versus a depth-buffer offset for cascades.
		view.CameraClip = glm::vec4(camera.Near, camera.Far, settings.ShadowBias, settings.ShadowNormalBias);
		// Fraction of the targets the scene covers: screen-space passes sample the scene's targets through it.
		const glm::vec2 uvScale(static_cast<float>(m_RenderWidth) / static_cast<float>(m_TargetWidth), static_cast<float>(m_RenderHeight) / static_cast<float>(m_TargetHeight));
		const float iblIntensity = skyLight ? skyLight->Intensity : 1.0f;
		view.AmbientColor = glm::vec4(settings.AmbientColor, iblIntensity);
		view.EnvironmentParams = glm::vec4(skyLight ? glm::radians(skyLight->Rotation) : 0.0f, static_cast<float>(PrefilteredMips), settings.SSAOEnabled ? 1.0f : 0.0f, static_cast<float>(bufferedLightCount));
		view.Sun = sun;
		view.ClusterGrid = glm::uvec4(LightClusterDimensions, m_ClusteredLighting ? 1u : 0u);
		view.ClusterDepths = LightClusterDepths(camera.Near, camera.Far, orthographic);
		view.ClusterDepths.w = m_VisualizeLightCount ? 1.0f : 0.0f;
		view.Quality = glm::vec4(graphics.ShadowQuality == GraphicsQuality::Low ? 1.0f : 0.0f, 0, 0, 0);

		const bool shadowsActive = sun.Direction.w > 0.5f && sun.Color.w > 0.5f;
		if (shadowsActive)
		{
			// Split the camera frustum (up to ShadowDistance) into cascades.
			const float nearClip = camera.Near;
			const float farClip = std::min(camera.Far, std::max(settings.ShadowDistance, nearClip + 1.0f));
			const auto splits = ComputeCascadeSplits<MaxCascades>(nearClip, farClip, settings.CascadeSplitLambda, orthographic);
			view.CascadeSplits = glm::vec4(splits[0], splits[1], splits[2], splits[3]);

			// Frustum corners at the camera's near and far planes.
			const glm::mat4 inverseViewProjection = glm::inverse(view.ViewProjection);
			glm::vec3 nearCorners[4];
			glm::vec3 farCorners[4];
			const glm::vec2 ndc[4] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
			for (int i = 0; i < 4; i++)
			{
				glm::vec4 nearPoint = inverseViewProjection * glm::vec4(ndc[i], 0.0f, 1.0f);
				glm::vec4 farPoint = inverseViewProjection * glm::vec4(ndc[i], 1.0f, 1.0f);
				nearCorners[i] = glm::vec3(nearPoint) / nearPoint.w;
				farCorners[i] = glm::vec3(farPoint) / farPoint.w;
			}

			const glm::vec3 lightDirection = glm::vec3(sun.Direction);
			const glm::vec3 up = ShadowUpVector(lightDirection);
			float previousSplit = nearClip;
			for (uint32_t cascade = 0; cascade < MaxCascades; cascade++)
			{
				// Corners of this cascade's frustum slice (view depth is linear along each corner ray).
				glm::vec3 corners[8];
				const float t0 = (previousSplit - camera.Near) / (camera.Far - camera.Near);
				const float t1 = (splits[cascade] - camera.Near) / (camera.Far - camera.Near);
				for (int i = 0; i < 4; i++)
				{
					corners[i] = glm::mix(nearCorners[i], farCorners[i], t0);
					corners[i + 4] = glm::mix(nearCorners[i], farCorners[i], t1);
				}
				previousSplit = splits[cascade];

				// Bounding sphere: rotation-invariant, so the shadow map does not swim as the camera turns.
				glm::vec3 center(0.0f);
				for (const glm::vec3& corner : corners)
					center += corner;
				center /= 8.0f;
				float radius = 0.0f;
				for (const glm::vec3& corner : corners)
					radius = std::max(radius, glm::length(corner - center));
				radius = std::ceil(radius * 16.0f) / 16.0f;

				constexpr float CasterDistance = 100.0f;
				const glm::mat4 lightView = glm::lookAt(center - lightDirection * (radius + CasterDistance), center, up);
				glm::mat4 lightProjection = glm::ortho(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + CasterDistance);

				// Snap to shadow texels so moving the camera does not shimmer the edges.
				const glm::mat4 shadowMatrix = lightProjection * lightView;
				glm::vec4 origin = shadowMatrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
				origin *= static_cast<float>(ShadowMapSize) / 2.0f;
				const glm::vec4 rounded = glm::round(origin);
				glm::vec4 offset = (rounded - origin) * 2.0f / static_cast<float>(ShadowMapSize);
				offset.z = 0.0f;
				offset.w = 0.0f;
				lightProjection[3] += offset;
				view.CascadeViewProjection[cascade] = lightProjection * lightView;
			}
		}

		// Meshes and textures stream in while the draw list is built; with the per-frame buffers they form the
		// Uploads pass, which dynamic resolution leaves out (no resolution would absorb a burst of uploads).
		std::optional<GpuPassScope> uploadsPass(std::in_place, profiler, commandList, PassUploads);
		commandList->writeBuffer(impl.ViewConstantsBuffer, &view, sizeof(view));

		// --- Draw list ----------------------------------------------------------------------------
		const Frustum frustum(view.ViewProjection);
		std::vector<Frustum> cascadeFrusta;
		std::vector<Frustum> localShadowFrusta;
		localShadowFrusta.reserve(localShadows.Maps.size());
		for (const auto& map : localShadows.Maps)
			localShadowFrusta.emplace_back(map.ViewProjection);
		if (shadowsActive)
		{
			for (const glm::mat4& cascadeViewProjection : view.CascadeViewProjection)
				cascadeFrusta.emplace_back(cascadeViewProjection);
		}
		std::vector<GpuDrawData>& draws = impl.Draws;
		std::vector<GpuMaterialData>& materials = impl.Materials;
		std::vector<DrawItem>& items = impl.Items;
		impl.PickEntities = { UUID(0) };
		std::unordered_map<UUID, uint32_t> pickIds;
		const std::unordered_set<UUID> selectedIds(m_Selection.begin(), m_Selection.end());
		draws.clear();
		materials.clear();
		items.clear();

		auto materialSet = [&](nvrhi::ITexture* albedo, nvrhi::ITexture* normal, nvrhi::ITexture* metallicRoughness, nvrhi::ITexture* occlusion, nvrhi::ITexture* emissive) -> nvrhi::IBindingSet* {
			const std::array<nvrhi::ITexture*, 5> key = { albedo, normal, metallicRoughness, occlusion, emissive };
			auto it = impl.MaterialSets.find(key);
			if (it != impl.MaterialSets.end())
			{
				it->second.LastUsed = impl.FrameCounter;
				return it->second.Set;
			}
			nvrhi::BindingSetHandle set = m_Device->createBindingSet(nvrhi::BindingSetDesc()
																		 .addItem(nvrhi::BindingSetItem::Texture_SRV(0, albedo))
																		 .addItem(nvrhi::BindingSetItem::Texture_SRV(1, normal))
																		 .addItem(nvrhi::BindingSetItem::Texture_SRV(2, metallicRoughness))
																		 .addItem(nvrhi::BindingSetItem::Texture_SRV(3, occlusion))
																		 .addItem(nvrhi::BindingSetItem::Texture_SRV(4, emissive))
																		 .addItem(nvrhi::BindingSetItem::Sampler(5, impl.LinearWrapAniso)),
																	 impl.MaterialLayout);
			impl.MaterialSets[key] = { set, impl.FrameCounter };
			return set;
		};

		// A texture that is missing or failed to load falls back (AssetManager and RenderResources log failures).
		auto resolveTexture = [&](const std::string& key, bool srgb, nvrhi::ITexture* fallback) -> nvrhi::ITexture* {
			if (key.empty())
				return fallback;
			Ref<TextureSource> source = m_AsyncLoading ? AssetManager::RequestTexture(key) : AssetManager::GetTexture(key);
			nvrhi::ITexture* gpu = source ? m_Resources->GetTexture(source, srgb, commandList) : nullptr;
			return gpu ? gpu : fallback;
		};
		auto texture = [&](const std::string& key, bool srgb, nvrhi::ITexture* fallback, uint32_t flag, uint32_t& flags) -> nvrhi::ITexture* {
			nvrhi::ITexture* resolved = resolveTexture(key, srgb, fallback);
			if (resolved != fallback)
				flags |= flag;
			return resolved;
		};

		const glm::vec3 cameraForward = -glm::vec3(view.InverseView[2]);
		auto addMesh = [&](Entity entity, const Ref<MeshSource>& source, uint32_t meshIndex, const glm::mat4& model, bool castShadows, const std::vector<glm::mat4>& palette, uint32_t& paletteOffset) {
			if (!source || meshIndex >= source->Meshes.size())
				return;
			const GpuMesh* gpuMesh = m_Resources->GetMesh(source, commandList);
			if (!gpuMesh)
				return;
			uint32_t pickId = 0;
			if (m_Picking)
			{
				if (impl.PickEntities.size() > MaxPickToken && !pickIds.contains(entity.GetUUID()))
				{
					BS_CORE_ERROR("SceneRenderer: picking token capacity exceeded");
					return;
				}
				auto [it, inserted] = pickIds.try_emplace(entity.GetUUID(), static_cast<uint32_t>(impl.PickEntities.size()));
				if (inserted)
					impl.PickEntities.push_back(entity.GetUUID());
				pickId = it->second | (selectedIds.contains(entity.GetUUID()) ? SelectedTokenBit : 0u);
			}
			const glm::mat4 normalMatrix = glm::transpose(glm::inverse(model));
			const auto* materialOverride = entity.TryGetComponent<MaterialComponent>();

			for (uint32_t submeshIndex : source->Meshes[meshIndex].Submeshes)
			{
				const Submesh& submesh = source->Submeshes[submeshIndex];
				const AABB bounds = palette.empty() ? submesh.Bounds : SkinBounds(*source, submesh, palette);
				const AABB worldBounds = bounds.Transformed(model);
				const bool visible = frustum.Intersects(worldBounds);
				auto intersectsAny = [&](const std::vector<Frustum>& frusta) {
					return std::any_of(frusta.begin(), frusta.end(), [&](const Frustum& f) { return f.Intersects(worldBounds); });
				};
				const bool castsShadow = castShadows && (intersectsAny(cascadeFrusta) || intersectsAny(localShadowFrusta));
				if (!visible && !castsShadow)
				{
					m_Statistics.CulledSubmeshes++;
					continue;
				}

				uint32_t flags = 0;
				GpuMaterialData material{};
				nvrhi::IBindingSet* set = nullptr;
				bool blend = false;
				bool doubleSided = false;
				if (materialOverride)
				{
					const MaterialComponent& m = *materialOverride;
					material.AlbedoColor = m.AlbedoColor;
					material.Emissive = glm::vec4(m.EmissiveColor * m.EmissiveIntensity, 0.0f);
					nvrhi::ITexture* albedo = texture(m.AlbedoMap, true, m_Resources->GetWhiteTexture(), MaterialHasAlbedoMap, flags);
					nvrhi::ITexture* normal = texture(m.NormalMap, false, m_Resources->GetFlatNormalTexture(), MaterialHasNormalMap, flags);
					nvrhi::ITexture* mr = texture(m.MetallicRoughnessMap, false, m_Resources->GetWhiteTexture(), MaterialHasMetallicRoughnessMap, flags);
					nvrhi::ITexture* occlusion = texture(m.OcclusionMap, false, m_Resources->GetWhiteTexture(), MaterialHasOcclusionMap, flags);
					nvrhi::ITexture* emissive = texture(m.EmissiveMap, true, m_Resources->GetWhiteTexture(), MaterialHasEmissiveMap, flags);
					material.Params = glm::vec4(m.Metallic, m.Roughness, 0.0f, static_cast<float>(flags));
					blend = m.AlbedoColor.a < 0.999f;
					set = materialSet(albedo, normal, mr, occlusion, emissive);
				}
				else
				{
					const MaterialData& m = source->Materials[std::min<size_t>(submesh.MaterialIndex, source->Materials.size() - 1)];
					material.AlbedoColor = m.AlbedoColor;
					material.Emissive = glm::vec4(m.EmissiveColor * m.EmissiveIntensity, 0.0f);
					nvrhi::ITexture* albedo = texture(m.AlbedoMap, true, m_Resources->GetWhiteTexture(), MaterialHasAlbedoMap, flags);
					nvrhi::ITexture* normal = texture(m.NormalMap, false, m_Resources->GetFlatNormalTexture(), MaterialHasNormalMap, flags);
					nvrhi::ITexture* mr = texture(m.MetallicRoughnessMap, false, m_Resources->GetWhiteTexture(), MaterialHasMetallicRoughnessMap, flags);
					nvrhi::ITexture* occlusion = texture(m.OcclusionMap, false, m_Resources->GetWhiteTexture(), MaterialHasOcclusionMap, flags);
					nvrhi::ITexture* emissive = texture(m.EmissiveMap, true, m_Resources->GetWhiteTexture(), MaterialHasEmissiveMap, flags);
					material.Params = glm::vec4(m.Metallic, m.Roughness, m.Alpha == AlphaMode::Mask ? m.AlphaCutoff : 0.0f, static_cast<float>(flags));
					blend = m.Alpha == AlphaMode::Blend;
					doubleSided = m.DoubleSided;
					set = materialSet(albedo, normal, mr, occlusion, emissive);
				}

				// Transparent geometry cannot keep an off-camera draw alive through shadow visibility.
				if (!visible && blend)
				{
					m_Statistics.CulledSubmeshes++;
					continue;
				}
				const uint32_t materialIndex = static_cast<uint32_t>(materials.size());
				materials.push_back(material);
				// Upload each instance's skin once, only if a camera or shadow pass needs it.
				// Palettes share the draw SSBO to retain the portable storage-buffer descriptor count.
				if (!palette.empty() && paletteOffset == UINT32_MAX)
				{
					paletteOffset = static_cast<uint32_t>(draws.size());
					for (const auto& joint : palette)
						draws.push_back({ joint, glm::mat4(1), glm::uvec4(0) });
					m_Statistics.SkinMatrices += static_cast<uint32_t>(palette.size());
				}
				const uint32_t drawIndex = static_cast<uint32_t>(draws.size());
				draws.push_back({ model, normalMatrix, glm::uvec4(materialIndex, pickId, paletteOffset, static_cast<uint32_t>(palette.size())) });

				DrawItem item;
				item.Mesh = gpuMesh;
				item.IndexCount = submesh.IndexCount;
				item.BaseIndex = submesh.BaseIndex;
				item.BaseVertex = submesh.BaseVertex;
				item.DrawIndex = drawIndex;
				item.MaterialSet = set;
				item.Blend = blend;
				item.DoubleSided = doubleSided;
				item.CastShadows = castsShadow && !blend;
				item.AlphaTested = material.Params.z > 0.0f;
				item.ShadowBounds = worldBounds;
				item.ViewDepth = visible ? glm::dot(worldBounds.GetCenter() - camera.Position, cameraForward) : -1.0f;
				item.WorldBounds = visible ? worldBounds : AABB{};
				items.push_back(item);
			}
		};
		uint32_t unusedPaletteOffset = 0;
		for (auto handle : scene.GetAllEntitiesWith<MeshComponent>())
		{
			Entity entity(handle, &scene);
			const auto& component = entity.GetComponent<MeshComponent>();
			auto source = m_AsyncLoading ? AssetManager::RequestMesh(component.Mesh) : AssetManager::GetMesh(component.Mesh);
			const bool placeholder = !source && m_AsyncLoading && AssetManager::IsLoading(component.Mesh);
			if (placeholder)
				source = m_PlaceholderMesh;
			addMesh(entity, source, placeholder ? 0 : component.MeshIndex, scene.GetWorldTransform(entity), component.CastShadows, {}, unusedPaletteOffset);
		}
		for (auto handle : scene.GetAllEntitiesWith<AnimatorComponent>())
		{
			Entity entity(handle, &scene);
			const auto& component = entity.GetComponent<AnimatorComponent>();
			std::unordered_map<int32_t, uint32_t> paletteOffsets;
			ForEachAnimatedMesh(entity, [&](const Ref<MeshSource>& source, const MeshNode& node, const glm::mat4& model, const std::vector<glm::mat4>& palette) {
				uint32_t& offset = node.SkinIndex < 0 ? unusedPaletteOffset : paletteOffsets.try_emplace(node.SkinIndex, UINT32_MAX).first->second;
				addMesh(entity, source, static_cast<uint32_t>(node.MeshIndex), model, component.CastShadows, palette, offset);
			});
		}
		m_Statistics.Materials = static_cast<uint32_t>(materials.size());

		// --- Particles ----------------------------------------------------------------------------
		// Only a running scene has a particle system. Batches outside the view frustum are not drawn.
		std::vector<ParticleDraw>& particleDraws = impl.ParticleDraws;
		particleDraws.clear();
		impl.ParticleData.Clear();
		if (const ParticleSystem* particleSystem = scene.GetParticleSystem())
			particleSystem->BuildRenderData(camera.Position, cameraForward, impl.ParticleData);
		auto limitParticles = [&](size_t budget) {
			const size_t total = impl.ParticleData.Particles.size();
			if (impl.ParticleData.LimitTo(budget))
				m_Statistics.ParticlesSkipped += static_cast<uint32_t>(total - impl.ParticleData.Particles.size());
		};
		limitParticles(MaxFrameParticles);
		// Reported once per overflow episode: a later overflow (after a frame within budget) warns again.
		if (m_Statistics.ParticlesSkipped > 0 && !impl.ReportedParticleCap)
			BS_CORE_WARN("SceneRenderer: particles exceed the {} drawn per frame; skipping {} in whole emitters", MaxFrameParticles, m_Statistics.ParticlesSkipped);
		impl.ReportedParticleCap = m_Statistics.ParticlesSkipped > 0;
		if (!impl.ParticleData.Particles.empty())
		{
			const nvrhi::BufferHandle grown = EnsureStructuredBuffer(m_Device, impl.ParticleBuffer, impl.ParticleData.Particles.size() * sizeof(GpuParticle), sizeof(GpuParticle), "ParticleBuffer");
			if (grown && grown != impl.ParticleBuffer)
			{
				impl.ParticleBuffer = grown;
				impl.ParticleMemory.Resize(static_cast<size_t>(grown->getDesc().byteSize));
				impl.ParticleSets.clear();
			}
			if (!grown)
			{
				// Keep the buffer that works and draw what fits in it.
				if (!impl.ReportedParticleBufferFailure)
					BS_CORE_ERROR("SceneRenderer: cannot create a buffer for {} particles; drawing the emitters that fit", impl.ParticleData.Particles.size());
				limitParticles(impl.ParticleBuffer ? impl.ParticleBuffer->getDesc().byteSize / sizeof(GpuParticle) : 0);
			}
			impl.ReportedParticleBufferFailure = !grown;
			if (!impl.ParticleData.Particles.empty())
				commandList->writeBuffer(impl.ParticleBuffer, impl.ParticleData.Particles.data(), impl.ParticleData.Particles.size() * sizeof(GpuParticle));
		}
		for (const ParticleBatch& batch : impl.ParticleData.Batches)
		{
			BS_CORE_ASSERT(batch.Count > 0 && batch.FirstParticle + batch.Count <= impl.ParticleData.Particles.size(), "particle batches index the particle array");
			BS_CORE_ASSERT(static_cast<size_t>(batch.Blend) < ParticleBlendModeCount, "render data carries sanitized blend modes");
			if (!frustum.Intersects(batch.Bounds))
				continue;
			nvrhi::ITexture* particleTexture = resolveTexture(batch.Texture, true, impl.ParticleTexture);
			Impl::CachedBindingSet& entry = impl.ParticleSets[particleTexture];
			if (!entry.Set)
			{
				entry.Set = m_Device->createBindingSet(nvrhi::BindingSetDesc()
														   .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(ParticleBufferSlot, impl.ParticleBuffer))
														   .addItem(nvrhi::BindingSetItem::Texture_SRV(ParticleTextureSlot, particleTexture))
														   .addItem(nvrhi::BindingSetItem::Sampler(ParticleSamplerSlot, impl.LinearClamp)),
													   impl.ParticleLayout);
				// Reported once per failure episode: a later failure (after a set was created) reports again.
				if (!entry.Set && !impl.ReportedParticleSetFailure)
					BS_CORE_ERROR("SceneRenderer: cannot create a particle binding set for texture '{}'; those particles are not drawn", batch.Texture.empty() ? "<soft disc>" : batch.Texture);
				impl.ReportedParticleSetFailure = !entry.Set;
			}
			if (!entry.Set)
			{
				impl.ParticleSets.erase(particleTexture);
				continue;
			}
			entry.LastUsed = impl.FrameCounter;
			particleDraws.push_back({ &batch, entry.Set });
			m_Statistics.Particles += batch.Count;
		}

		// Upload per-frame buffers (empty arrays still need one element for valid descriptors).
		if (draws.empty())
			draws.push_back({});
		if (materials.empty())
			materials.push_back({});
		if (lights.empty())
			lights.push_back({});
		const nvrhi::BufferHandle previousDraws = impl.DrawBuffer;
		const nvrhi::BufferHandle previousMaterials = impl.MaterialBuffer;
		impl.DrawBuffer = EnsureStructuredBuffer(m_Device, impl.DrawBuffer, draws.size() * sizeof(GpuDrawData), sizeof(GpuDrawData), "DrawBuffer");
		impl.DrawMemory.Resize(impl.DrawBuffer ? static_cast<size_t>(impl.DrawBuffer->getDesc().byteSize) : 0);
		impl.MaterialBuffer = EnsureStructuredBuffer(m_Device, impl.MaterialBuffer, materials.size() * sizeof(GpuMaterialData), sizeof(GpuMaterialData), "MaterialBuffer");
		impl.MaterialMemory.Resize(impl.MaterialBuffer ? static_cast<size_t>(impl.MaterialBuffer->getDesc().byteSize) : 0);
		if (!impl.DrawBuffer || !impl.MaterialBuffer)
		{
			if (!impl.ReportedDrawBufferFailure)
				BS_CORE_ERROR("SceneRenderer: failed to allocate draw/palette ({} bytes) or material buffer ({} bytes)", draws.size() * sizeof(GpuDrawData), materials.size() * sizeof(GpuMaterialData));
			impl.ReportedDrawBufferFailure = true;
			uploadsPass.reset();
			abortFrame();
			return;
		}
		impl.ReportedDrawBufferFailure = false;
		if (impl.DrawBuffer != previousDraws || impl.MaterialBuffer != previousMaterials)
			impl.FrameSetDirty = true;
		commandList->writeBuffer(impl.DrawBuffer, draws.data(), draws.size() * sizeof(GpuDrawData));
		commandList->writeBuffer(impl.MaterialBuffer, materials.data(), materials.size() * sizeof(GpuMaterialData));
		commandList->writeBuffer(impl.LightBuffer, lights.data(), lights.size() * sizeof(GpuLightData));
		if (!localShadows.Maps.empty())
			commandList->writeBuffer(impl.LocalShadowBuffer, localShadows.Maps.data(), localShadows.Maps.size() * sizeof(LocalShadowMap));

		if (impl.FrameSetDirty)
		{
			nvrhi::BindingSetDesc commonFrameDesc;
			commonFrameDesc.addItem(nvrhi::BindingSetItem::PushConstants(PushConstantSlot, sizeof(DrawPush)))
				.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, impl.ViewConstantsBuffer))
				.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, impl.DrawBuffer))
				.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, impl.MaterialBuffer));
			auto lightingDesc = commonFrameDesc;
			impl.FrameSet = m_Device->createBindingSet(lightingDesc
														   .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, impl.LightBuffer))
														   .addItem(nvrhi::BindingSetItem::Texture_SRV(4, impl.ShadowMap, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::Texture2DArray))
														   .addItem(nvrhi::BindingSetItem::Texture_SRV(5, impl.Env.Irradiance, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::TextureCube))
														   .addItem(nvrhi::BindingSetItem::Texture_SRV(6, impl.Env.Prefiltered, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::TextureCube))
														   .addItem(nvrhi::BindingSetItem::Texture_SRV(7, impl.BrdfLut))
														   .addItem(nvrhi::BindingSetItem::Texture_SRV(8, impl.SsaoTexture))
														   .addItem(nvrhi::BindingSetItem::Sampler(9, impl.LinearClamp))
														   .addItem(nvrhi::BindingSetItem::Sampler(10, impl.ShadowCompare))
														   .addItem(nvrhi::BindingSetItem::Sampler(11, impl.PointClamp))
														   .addItem(nvrhi::BindingSetItem::Texture_SRV(12, impl.Env.Cube, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::TextureCube))
														   .addItem(nvrhi::BindingSetItem::Texture_SRV(13, impl.LocalShadowAtlas))
														   .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(14, impl.LocalShadowBuffer)),
													   impl.FrameLayout);
			impl.ShadowFrameSet = m_Device->createBindingSet(commonFrameDesc
																 .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(14, impl.LocalShadowBuffer)),
															 impl.ShadowFrameLayout);
			if (!impl.FrameSet || !impl.ShadowFrameSet)
			{
				if (!impl.ReportedFrameSetFailure)
					BS_CORE_ERROR("SceneRenderer: failed to create {} frame bindings", !impl.FrameSet ? "lighting" : "shadow");
				impl.ReportedFrameSetFailure = true;
				uploadsPass.reset();
				abortFrame();
				return;
			}
			impl.ReportedFrameSetFailure = false;
			impl.FrameSetDirty = false;
		}
		uploadsPass.reset();

		// The scene targets can be larger than this frame's render area (dynamic resolution), so their passes use
		// the render size; shadow maps use their own.
		const nvrhi::Viewport renderViewport(static_cast<float>(m_RenderWidth), static_cast<float>(m_RenderHeight));
		const nvrhi::Viewport shadowViewport(static_cast<float>(ShadowMapSize), static_cast<float>(ShadowMapSize));
		auto drawItem = [&](const DrawItem& item, nvrhi::IGraphicsPipeline* pipeline, nvrhi::IFramebuffer* framebuffer, const nvrhi::Viewport& viewport, uint32_t shadowViewIndex, bool withMaterial, nvrhi::IBindingSet* frameSet = nullptr) {
			nvrhi::GraphicsState state;
			state.pipeline = pipeline;
			state.framebuffer = framebuffer;
			state.bindings = { frameSet ? frameSet : impl.FrameSet.Get() };
			if (withMaterial)
				state.bindings.push_back(item.MaterialSet);
			state.vertexBuffers = { nvrhi::VertexBufferBinding().setBuffer(item.Mesh->VertexBuffer).setSlot(0).setOffset(0) };
			state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(item.Mesh->IndexBuffer).setFormat(nvrhi::Format::R32_UINT).setOffset(0);
			state.viewport.addViewportAndScissorRect(viewport);
			commandList->setGraphicsState(state);
			const DrawPush push{ item.DrawIndex, shadowViewIndex };
			commandList->setPushConstants(&push, sizeof(push));
			nvrhi::DrawArguments args;
			args.vertexCount = item.IndexCount;
			args.startIndexLocation = item.BaseIndex;
			args.startVertexLocation = item.BaseVertex;
			commandList->drawIndexed(args);
		};

		auto fullscreen = [&](nvrhi::IGraphicsPipeline* pipeline, nvrhi::IFramebuffer* framebuffer, nvrhi::IBindingSet* set, const nvrhi::Viewport& viewport, const void* push, size_t pushSize) {
			DrawFullscreen(commandList, pipeline, framebuffer, set, viewport, push, pushSize);
		};
		const nvrhi::Viewport outputViewport(static_cast<float>(m_Width), static_cast<float>(m_Height));

		// --- Shadow maps --------------------------------------------------------------------------
		{
			const GpuPassScope pass(profiler, commandList, PassShadows);
			auto drawShadowCasters = [&](const Frustum& shadowFrustum, nvrhi::IGraphicsPipeline* opaque,
										 nvrhi::IGraphicsPipeline* masked, nvrhi::IFramebuffer* framebuffer, const nvrhi::Viewport& viewport, uint32_t index) {
				for (const DrawItem& item : items)
				{
					if (!item.CastShadows || !shadowFrustum.Intersects(item.ShadowBounds))
						continue;
					drawItem(item, item.AlphaTested ? masked : opaque, framebuffer, viewport, index, item.AlphaTested, impl.ShadowFrameSet);
					m_Statistics.ShadowDrawCalls++;
				}
			};
			commandList->clearDepthStencilTexture(impl.ShadowMap, nvrhi::AllSubresources, true, 1.0f, false, 0);
			if (shadowsActive)
				for (uint32_t cascade = 0; cascade < MaxCascades; cascade++)
					drawShadowCasters(cascadeFrusta[cascade], impl.ShadowPipeline, impl.ShadowMaskedPipeline, impl.ShadowFramebuffers[cascade], shadowViewport, cascade);
			// With no maps every light has Params.y == 0, so no shader samples the never-written fallback
			// or a previous frame's atlas. nvrhi clears whole depth subresources, not tile rectangles.
			if (!localShadows.Maps.empty())
				commandList->clearDepthStencilTexture(impl.LocalShadowAtlas, nvrhi::AllSubresources, true, 1.0f, false, 0);
			for (uint32_t map = 0; map < localShadows.Maps.size(); map++)
			{
				const glm::vec4 rect = localShadows.Maps[map].AtlasRect * static_cast<float>(LocalShadowAtlasSize);
				const nvrhi::Viewport viewport(rect.z, rect.z + rect.x, rect.w, rect.w + rect.y, 0.0f, 1.0f);
				drawShadowCasters(localShadowFrusta[map], impl.LocalShadowPipeline, impl.LocalShadowMaskedPipeline, impl.LocalShadowFramebuffer, viewport, MaxCascades + map);
			}
		}

		// --- Depth/normal prepass (opaque, visible) -----------------------------------------------
		{
			const GpuPassScope pass(profiler, commandList, PassPrepass);
			// Clears cover the whole targets, also outside a dynamic-resolution render area; nothing reads there.
			if (m_Picking)
				commandList->clearTextureUInt(impl.EntityIds, nvrhi::AllSubresources, 0);
			commandList->clearDepthStencilTexture(m_DepthTexture, nvrhi::AllSubresources, true, 1.0f, false, 0);
			commandList->clearTextureFloat(impl.NormalTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 1.0f, 0.0f));
			for (const DrawItem& item : items)
			{
				if (item.Blend || !item.WorldBounds.IsValid())
					continue;
				drawItem(item, impl.PrepassPipeline[item.DoubleSided ? 1 : 0], impl.PrepassFramebuffer, renderViewport, 0, true);
			}
		}

		// Transparent selection gets its own nearest-surface depth, leaving lighting/SSAO depth intact.
		if (m_Picking)
		{
			commandList->copyTexture(impl.PickDepth, nvrhi::TextureSlice(), m_DepthTexture, nvrhi::TextureSlice());
			for (const DrawItem& item : items)
				if (item.Blend && item.WorldBounds.IsValid())
					drawItem(item, impl.PickPipeline[item.DoubleSided ? 1 : 0], impl.PickFramebuffer, renderViewport, 0, true);
		}

		// --- SSAO ---------------------------------------------------------------------------------
		if (settings.SSAOEnabled)
		{
			const GpuPassScope pass(profiler, commandList, PassSSAO);
			impl.SsaoConstants.Projection = camera.Projection;
			impl.SsaoConstants.InverseProjection = view.InverseProjection;
			impl.SsaoConstants.Params = glm::vec4(settings.SSAORadius, settings.SSAOBias, settings.SSAOIntensity, graphics.SSAOQuality == GraphicsQuality::Low ? 8.0f : static_cast<float>(SSAOKernelSize));
			impl.SsaoConstants.ScreenSize = glm::vec4(static_cast<float>(m_RenderWidth), static_cast<float>(m_RenderHeight), static_cast<float>(m_RenderWidth) / 4.0f, static_cast<float>(m_RenderHeight) / 4.0f);
			impl.SsaoConstants.UVScale = glm::vec4(uvScale, 0.0f, 0.0f);
			commandList->writeBuffer(impl.SsaoConstantsBuffer, &impl.SsaoConstants, sizeof(GpuSSAOConstants));

			fullscreen(impl.SsaoPipeline, impl.SsaoFramebuffer, impl.SsaoSet, renderViewport, nullptr, 0);
			const float orthographicFlag = orthographic ? 1.0f : 0.0f;
			// Blur steps are one target texel: the targets, not the render area, set the texel size.
			BlurPush blur{ { 1.0f / static_cast<float>(m_TargetWidth), 0.0f }, camera.Near, camera.Far, orthographicFlag, 0.0f, uvScale };
			fullscreen(impl.SsaoBlurPipeline, impl.SsaoBlurFramebuffer, impl.BlurHorizontalSet, renderViewport, &blur, sizeof(blur));
			blur.Direction = { 0.0f, 1.0f / static_cast<float>(m_TargetHeight) };
			fullscreen(impl.SsaoBlurPipeline, impl.SsaoFramebuffer, impl.BlurVerticalSet, renderViewport, &blur, sizeof(blur));
		}
		else
		{
			// Debug views must show disabled occlusion, not the previous enabled frame.
			commandList->clearTextureFloat(impl.SsaoTexture, nvrhi::AllSubresources, nvrhi::Color(1.0f));
		}

		// Build against the complete frustum, not the prepass depth: transparent surfaces need lists too.
		if (m_ClusteredLighting)
		{
			bool built = false;
			{
				const GpuPassScope pass(profiler, commandList, PassClusters);
				built = impl.Clusters.Build(commandList, camera.View, camera.Projection, view.ClusterDepths, bufferedLightCount);
			}
			if (!built)
			{
				abortFrame();
				return;
			}
		}

		// --- Lighting (opaque), skybox, transparent -----------------------------------------------
		{
			const GpuPassScope pass(profiler, commandList, PassLighting);
			commandList->clearTextureFloat(impl.HdrTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
			for (const DrawItem& item : items)
			{
				if (item.Blend || !item.WorldBounds.IsValid())
					continue;
				drawItem(item, impl.OpaquePipeline[item.DoubleSided ? 1 : 0], impl.LightingFramebuffer, renderViewport, 0, true);
				m_Statistics.DrawCalls++;
			}

			if (!skyLight || skyLight->ShowBackground)
			{
				const DrawPush push{ 0, 0 };
				fullscreen(impl.SkyboxPipeline, impl.LightingFramebuffer, impl.FrameSet, renderViewport, &push, sizeof(push));
			}

			// Blended meshes and particle batches, back to front. Particles sort as one batch per emitter (by
			// their bounds center), like a mesh; within the batch they are already sorted.
			std::vector<TransparentDraw>& transparent = impl.Transparent;
			transparent.clear();
			for (const DrawItem& item : items)
			{
				if (item.Blend && item.WorldBounds.IsValid())
					transparent.push_back({ item.ViewDepth, &item });
			}
			for (const ParticleDraw& particleDraw : particleDraws)
				transparent.push_back({ particleDraw.Batch->ViewDepth, &particleDraw });
			// Stable: draws at equal depth keep submission order, so a scene always renders the same image.
			std::stable_sort(transparent.begin(), transparent.end(), [](const TransparentDraw& a, const TransparentDraw& b) { return a.ViewDepth > b.ViewDepth; });
			auto drawParticles = [&](const ParticleDraw& particleDraw) {
				const ParticleBatch& batch = *particleDraw.Batch;
				nvrhi::GraphicsState state;
				state.pipeline = impl.ParticlePipeline[static_cast<size_t>(batch.Blend)];
				state.framebuffer = impl.LightingFramebuffer;
				state.bindings = { impl.FrameSet, particleDraw.Set };
				state.viewport.addViewportAndScissorRect(renderViewport);
				commandList->setGraphicsState(state);
				// Particle.vert reads DrawPush.DrawIndex as the batch's first particle.
				const DrawPush push{ batch.FirstParticle, 0 };
				commandList->setPushConstants(&push, sizeof(push));
				// Six vertices (two triangles) per billboard, pulled from the particle buffer.
				commandList->draw(nvrhi::DrawArguments().setVertexCount(batch.Count * 6));
			};
			// std::visit: a new kind of transparent draw must be handled here to compile.
			const auto drawTransparent = Overloaded{
				[&](const DrawItem* item) { drawItem(*item, impl.TransparentPipeline[item->DoubleSided ? 1 : 0], impl.LightingFramebuffer, renderViewport, 0, true); },
				[&](const ParticleDraw* particleDraw) { drawParticles(*particleDraw); },
			};
			for (const TransparentDraw& draw : transparent)
			{
				std::visit(drawTransparent, draw.Source);
				m_Statistics.DrawCalls++;
			}
		}

		const bool postActive = settings.DebugView == RendererDebugView::None && (post.BloomEnabled || post.AutoExposure || post.FXAAEnabled);
		const bool fxaa = postActive && post.FXAAEnabled;
		const bool postActivated = postActive && !impl.PostProcessWasActive;
		impl.PostProcessWasActive = postActive;
		nvrhi::ITexture* tonemapSource = impl.HdrTexture;
		if (postActive)
		{
			if (postActivated)
			{
				impl.PostProcess.Retry();
				impl.TonemapSetFailed = false;
				impl.FxaaTonemapFailed = false;
			}
			if (!impl.PostProcess.Prepare(m_Device, impl.HdrTexture, impl.FullscreenVS, impl.LinearClamp))
			{
				abortFrame();
				return;
			}
			const GpuPassScope pass(profiler, commandList, PassPostProcess);
			const bool resetExposure = postActivated || impl.ExposureRevision != scene.GetRenderRevision() || impl.ExposureComponent != postProcessId;
			impl.PostProcess.RenderHDR(commandList, { m_RenderWidth, m_RenderHeight }, post, deltaTime, resetExposure);
			impl.ExposureRevision = scene.GetRenderRevision();
			impl.ExposureComponent = postProcessId;
			tonemapSource = impl.PostProcess.GetHDR();
		}
		if (impl.TonemapSource != tonemapSource)
		{
			if (impl.TonemapSetFailed)
			{
				abortFrame();
				return;
			}
			const auto tonemapSet = impl.CreateTonemapSet(m_Device, tonemapSource, m_DepthTexture);
			if (!tonemapSet)
			{
				impl.TonemapSetFailed = true;
				BS_CORE_ERROR("SceneRenderer: failed to create post-process tonemap bindings");
				abortFrame();
				return;
			}
			impl.TonemapSet = tonemapSet;
			impl.TonemapSource = tonemapSource;
		}

		// The current render area selects FSR, even when a dynamic run starts above native size.
		const bool fsr1 = settings.FSR1Enabled && settings.DebugView == RendererDebugView::None &&
						  NeedsSpatialUpscale({ m_RenderWidth, m_RenderHeight }, { m_Width, m_Height });
		impl.Upscaler.SetActive(fsr1);
		if (fsr1 && !impl.FsrRequested)
			impl.FsrTonemapFailed = false;
		impl.FsrRequested = fsr1;
		if (fsr1)
		{
			const glm::uvec2 fsrTargetSize = ComputeFsr1TargetSize({ m_TargetWidth, m_TargetHeight }, { m_Width, m_Height });
			if (!impl.Upscaler.Prepare(m_Device, fsrTargetSize, impl.OutputFramebuffer, impl.FullscreenVS, impl.PointClamp))
			{
				abortFrame();
				return;
			}
			if (!fxaa && !impl.FsrTonemapPipeline && !impl.FsrTonemapFailed)
			{
				impl.FsrTonemapPipeline = CreateFullscreenPipeline(m_Device, impl.FullscreenVS, impl.TonemapPS, impl.TonemapLayout, impl.Upscaler.GetInputFramebuffer());
				if (!impl.FsrTonemapPipeline)
				{
					BS_CORE_ERROR("SceneRenderer: failed to create FSR 1 tonemap pipeline (target {}x{}, output {}x{})", fsrTargetSize.x, fsrTargetSize.y, m_Width, m_Height);
					impl.FsrTonemapFailed = true;
				}
			}
			if (!fxaa && impl.FsrTonemapFailed)
			{
				abortFrame();
				return;
			}
		}

		if (fxaa)
		{
			if (!impl.PostProcess.PrepareFXAA(fsr1 ? impl.Upscaler.GetInputFramebuffer() : impl.OutputFramebuffer.Get()))
			{
				abortFrame();
				return;
			}
			if (!impl.FxaaTonemapPipeline && !impl.FxaaTonemapFailed)
				impl.FxaaTonemapPipeline = CreateFullscreenPipeline(m_Device, impl.FullscreenVS, impl.TonemapPS, impl.TonemapLayout, impl.PostProcess.GetFXAAInput());
			if (!impl.FxaaTonemapPipeline)
			{
				if (!impl.FxaaTonemapFailed)
					BS_CORE_ERROR("SceneRenderer: failed to create FXAA tonemap pipeline");
				impl.FxaaTonemapFailed = true;
				abortFrame();
				return;
			}
		}

		// --- Tonemap + optional FSR 1 (included in the dynamic-resolution GPU cost) -----------------
		{
			const GpuPassScope pass(profiler, commandList, PassTonemap);
			const uint32_t outputMode = (fsr1 || fxaa) ? 1u : (m_RenderWidth == m_Width && m_RenderHeight == m_Height ? 2u : 0u);
			const TonemapPush push = { settings.Exposure, static_cast<uint32_t>(settings.Tonemap), static_cast<uint32_t>(settings.DebugView), outputMode, uvScale };
			const auto tonemapPipeline = fxaa ? impl.FxaaTonemapPipeline : (fsr1 ? impl.FsrTonemapPipeline : impl.TonemapPipeline);
			nvrhi::IFramebuffer* tonemapTarget = fxaa ? impl.PostProcess.GetFXAAInput() : (fsr1 ? impl.Upscaler.GetInputFramebuffer() : impl.OutputFramebuffer.Get());
			fullscreen(tonemapPipeline, tonemapTarget, impl.TonemapSet, (fxaa || fsr1) ? renderViewport : outputViewport, &push, sizeof(push));
			if (fxaa)
				impl.PostProcess.RenderFXAA(commandList, fsr1 ? impl.Upscaler.GetInputFramebuffer() : impl.OutputFramebuffer.Get(), { m_RenderWidth, m_RenderHeight }, !fsr1);
			if (fsr1)
			{
				impl.Upscaler.Render(commandList, impl.OutputFramebuffer, { m_RenderWidth, m_RenderHeight }, settings.FSR1Sharpness);
				m_Statistics.FSR1Active = true;
			}
		}

		// --- Overlay depth (scaled or dynamic-resolution targets only) -----------------------------
		// Every frame rather than only when lines are drawn, so the overlay framebuffer always holds the
		// frame's depth. Point sampling keeps silhouettes hard: a blend of foreground and background depth
		// would be a surface that does not exist.
		if (impl.DepthUpsamplePipeline)
		{
			const GpuPassScope pass(profiler, commandList, PassOverlayDepth);
			fullscreen(impl.DepthUpsamplePipeline, impl.OverlayDepthFramebuffer, impl.DepthUpsampleSet, outputViewport, &uvScale, sizeof(uvScale));
		}

		// --- Debug lines (scripts, editor gizmos) --------------------------------------------------
		const std::vector<DebugLineVertex> lines = DebugDraw::TakeLines();
		if (!lines.empty())
		{
			const GpuPassScope pass(profiler, commandList, PassDebugLines);
			impl.Lines.Render(commandList, m_OverlayFramebuffer, view.ViewProjection, lines);
		}

		// One outline pass for the union of selected surfaces, independent of selection size.
		if (m_Picking && !m_Selection.empty())
		{
			const OutlinePush push{ { m_RenderWidth, m_RenderHeight }, { m_Width, m_Height }, SelectedTokenBit, 0 };
			fullscreen(impl.OutlinePipeline, impl.OutputFramebuffer, impl.OutlineSet, outputViewport, &push, sizeof(push));
		}

		profiler.EndFrame(commandList, static_cast<uint64_t>(m_RenderWidth) * m_RenderHeight);
		commandList->close();
		m_Device->executeCommandList(commandList);

		impl.PickReady = m_Picking;

		// Release cached binding sets (and the textures they reference) that are no longer used.
		auto isStale = [&](const auto& entry) { return impl.FrameCounter - entry.second.LastUsed > MaterialSetRetentionFrames; };
		std::erase_if(impl.MaterialSets, isStale);
		std::erase_if(impl.ParticleSets, isStale);
		if (impl.FrameCounter % 300 == 0)
			m_Resources->CollectGarbage();
	}

	std::optional<UITexture> SceneRenderer::GetUITexture(const std::string& path, nvrhi::ICommandList* commandList)
	{
		Ref<TextureSource> source = m_AsyncLoading ? AssetManager::RequestTexture(path) : AssetManager::GetTexture(path);
		if (!source)
			return std::nullopt;
		nvrhi::ITexture* texture = m_Resources->GetTexture(source, false, commandList);
		if (!texture)
			return std::nullopt;
		return UITexture{ reinterpret_cast<uint64_t>(texture), { static_cast<float>(source->Width), static_cast<float>(source->Height) } };
	}

	void SceneRenderer::Blit(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* target)
	{
		Impl& impl = *m_Impl;
		if (!m_OutputTexture || !target)
			return;

		const nvrhi::FramebufferInfoEx& info = target->getFramebufferInfo();
		std::string key = std::to_string(static_cast<int>(info.colorFormats.empty() ? nvrhi::Format::UNKNOWN : info.colorFormats[0]));
		auto it = impl.BlitPipelines.find(key);
		if (it == impl.BlitPipelines.end())
		{
			nvrhi::GraphicsPipelineDesc desc;
			desc.VS = impl.FullscreenVS;
			desc.PS = impl.BlitPS;
			desc.renderState.rasterState.setCullNone();
			desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
			desc.bindingLayouts = { impl.TextureSamplerLayout };
			it = impl.BlitPipelines.emplace(key, m_Device->createGraphicsPipeline(desc, info)).first;
		}

		const glm::vec2 unused(0.0f);
		DrawFullscreen(commandList, it->second, target, impl.BlitSet, nvrhi::Viewport(static_cast<float>(info.width), static_cast<float>(info.height)), &unused, TextureSamplerPushSize);
	}

	void SceneRenderer::InvalidatePicking()
	{
		m_Impl->PickReady = false;
		m_Impl->PickEntities.clear();
	}

	std::optional<UUID> SceneRenderer::Pick(uint32_t x, uint32_t y)
	{
		const auto sample = PickSurface(x, y);
		return sample ? std::optional<UUID>(sample->EntityId) : std::nullopt;
	}

	std::optional<ViewportPick> SceneRenderer::PickSurface(uint32_t x, uint32_t y)
	{
		Impl& impl = *m_Impl;
		if (!impl.PickReady || m_Statistics.Failed || x >= m_Width || y >= m_Height)
			return std::nullopt;
		nvrhi::TextureSlice source;
		source.x = std::min(static_cast<uint32_t>((static_cast<double>(x) + 0.5) * m_RenderWidth / m_Width), m_RenderWidth - 1);
		source.y = std::min(static_cast<uint32_t>((static_cast<double>(y) + 0.5) * m_RenderHeight / m_Height), m_RenderHeight - 1);
		source.width = source.height = source.depth = 1;
		std::vector<ReadbackRegion> regions{ { impl.EntityIds, source, {} }, { impl.PickDepth, source, {} } };
		if (!ReadTextureRegions(m_Device, regions))
			return std::nullopt;
		uint32_t token = 0;
		float depth = 1;
		std::memcpy(&token, regions[0].Pixels.data(), sizeof(token));
		std::memcpy(&depth, regions[1].Pixels.data(), sizeof(depth));
		token &= ~SelectedTokenBit;
		if (token >= impl.PickEntities.size() || !std::isfinite(depth) || depth < 0 || depth > 1)
		{
			BS_CORE_ERROR("SceneRenderer: invalid picking token/depth {}/{}", token, depth);
			return std::nullopt;
		}
		return ViewportPick{ impl.PickEntities[token], depth, { (static_cast<float>(source.x) + 0.5f) / static_cast<float>(m_RenderWidth), (static_cast<float>(source.y) + 0.5f) / static_cast<float>(m_RenderHeight) } };
	}

	bool SceneRenderer::ReadOutputPixels(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight)
	{
		if (!m_OutputTexture)
			return false;
		nvrhi::TextureSlice slice;
		slice.width = m_Width;
		slice.height = m_Height;
		slice.depth = 1;
		std::vector<ReadbackRegion> regions{ { m_OutputTexture, slice, {} } };
		if (!ReadTextureRegions(m_Device, regions))
			return false;
		outWidth = m_Width;
		outHeight = m_Height;
		outPixels = std::move(regions[0].Pixels);
		return true;
	}

}
