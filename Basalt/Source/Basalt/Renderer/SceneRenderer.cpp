#include "Basalt/Renderer/SceneRenderer.h"

#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Renderer/DebugLineRenderer.h"
#include "Basalt/Renderer/RenderMath.h"
#include "Basalt/Renderer/ShaderUtils.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <nvrhi/utils.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <random>

namespace Basalt {

	namespace {

		constexpr uint32_t MaxCascades = 4;
		constexpr uint32_t ShadowMapSize = 2048;
		constexpr uint32_t EnvironmentSize = 512;
		constexpr uint32_t IrradianceSize = 32;
		constexpr uint32_t PrefilteredSize = 128;
		constexpr uint32_t PrefilteredMips = 6;
		constexpr uint32_t BRDFLutSize = 256;
		constexpr uint32_t SSAOKernelSize = 24;
		constexpr uint32_t MaxLights = 256;
		constexpr uint64_t MaterialSetRetentionFrames = 120;

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
		};
		static_assert(sizeof(GpuViewConstants) == 720, "GpuViewConstants must match the std140 ViewConstants block");

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

		struct GpuLightData
		{
			glm::vec4 Position;
			glm::vec4 Color;
			glm::vec4 Direction;
			glm::vec4 Params;
		};
		static_assert(sizeof(GpuLightData) == 64);

		struct GpuSSAOConstants
		{
			glm::mat4 Projection;
			glm::mat4 InverseProjection;
			glm::vec4 Kernel[32];
			glm::vec4 Params;
			glm::vec4 ScreenSize;
		};

		struct DrawPush
		{
			uint32_t DrawIndex;
			uint32_t CascadeIndex;
		};

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

		nvrhi::BindingLayoutDesc MakeLayout(uint32_t set, nvrhi::ShaderType visibility)
		{
			nvrhi::BindingLayoutDesc desc;
			desc.visibility = visibility;
			desc.bindingOffsets = ZeroBindingOffsets;
			desc.registerSpace = set;
			desc.registerSpaceIsDescriptorSet = true;
			return desc;
		}

	}

	struct SceneRenderer::Impl
	{
		nvrhi::CommandListHandle CommandList;
		DebugLineRenderer Lines;

		// Shaders
		nvrhi::ShaderHandle ShadowMaskedVS, ShadowMaskedPS;
		nvrhi::ShaderHandle MeshVS, DepthNormalPS, PbrPS, ShadowVS, FullscreenVS, SsaoPS, SsaoBlurPS, SkyboxVS, SkyboxPS, TonemapPS, BlitPS;
		nvrhi::ShaderHandle EquirectCS, SkyCS, IrradianceCS, PrefilterCS, BrdfCS, DownsampleCS;

		// Layouts
		nvrhi::InputLayoutHandle MeshInputLayout;
		nvrhi::InputLayoutHandle ShadowInputLayout;
		nvrhi::InputLayoutHandle ShadowMaskedInputLayout;
		nvrhi::BindingLayoutHandle FrameLayout;
		nvrhi::BindingLayoutHandle MaterialLayout;
		nvrhi::BindingLayoutHandle SsaoLayout;
		nvrhi::BindingLayoutHandle BlurLayout;
		nvrhi::BindingLayoutHandle TextureSamplerLayout; // blit: texture(0), sampler(1)
		nvrhi::BindingLayoutHandle TonemapLayout;        // HDR(0), sampler(1), SSAO(2), normals(3), depth(4)
		nvrhi::BindingLayoutHandle ComputeLayout;        // IBL: texture(0), sampler(1), image(2)

		// Samplers
		nvrhi::SamplerHandle EquirectSampler, LinearClamp, LinearWrapAniso, PointClamp, PointWrap, ShadowCompare;

		// Constant GPU resources
		nvrhi::BufferHandle ViewConstantsBuffer;
		nvrhi::BufferHandle SsaoConstantsBuffer;
		nvrhi::BufferHandle DrawBuffer, MaterialBuffer, LightBuffer;
		nvrhi::TextureHandle ShadowMap;
		std::array<nvrhi::FramebufferHandle, MaxCascades> ShadowFramebuffers;
		nvrhi::TextureHandle SsaoNoise;
		nvrhi::TextureHandle BrdfLut;
		bool BrdfLutReady = false;
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

		// Size-dependent targets
		nvrhi::TextureHandle NormalTexture, HdrTexture, SsaoTexture, SsaoBlurTexture;
		nvrhi::FramebufferHandle PrepassFramebuffer, LightingFramebuffer, SsaoFramebuffer, SsaoBlurFramebuffer, OutputFramebuffer;

		// Pipelines (created lazily per framebuffer layout where needed)
		nvrhi::GraphicsPipelineHandle PrepassPipeline[2]; // [doubleSided]
		nvrhi::GraphicsPipelineHandle OpaquePipeline[2];
		nvrhi::GraphicsPipelineHandle TransparentPipeline[2];
		nvrhi::GraphicsPipelineHandle ShadowPipeline;
		nvrhi::GraphicsPipelineHandle ShadowMaskedPipeline;
		nvrhi::GraphicsPipelineHandle SkyboxPipeline;
		nvrhi::GraphicsPipelineHandle SsaoPipeline;
		nvrhi::GraphicsPipelineHandle SsaoBlurPipeline;
		nvrhi::GraphicsPipelineHandle TonemapPipeline;
		std::unordered_map<std::string, nvrhi::GraphicsPipelineHandle> BlitPipelines;
		nvrhi::ComputePipelineHandle EquirectPipeline, SkyPipeline, IrradiancePipeline, PrefilterPipeline, BrdfPipeline, DownsamplePipeline;

		// Binding sets
		nvrhi::BindingSetHandle FrameSet;
		nvrhi::BindingSetHandle SsaoSet, BlurHorizontalSet, BlurVerticalSet, TonemapSet, BlitSet;
		struct MaterialSetEntry
		{
			nvrhi::BindingSetHandle Set;
			uint64_t LastUsed = 0;
		};
		// Keyed by the five material textures (albedo, normal, metallic-roughness, occlusion, emissive).
		std::map<std::array<nvrhi::ITexture*, 5>, MaterialSetEntry> MaterialSets;

		// Per-frame scratch, kept to avoid reallocating every frame.
		std::vector<GpuDrawData> Draws;
		std::vector<GpuMaterialData> Materials;
		std::vector<GpuLightData> Lights;
		std::vector<DrawItem> Items;
		std::vector<const DrawItem*> Transparent;
		uint64_t FrameCounter = 0;
		bool FrameSetDirty = true;
	};

	SceneRenderer::SceneRenderer(nvrhi::IDevice* device)
		: m_Impl(CreateScope<Impl>())
		, m_Device(device)
		, m_Resources(CreateScope<RenderResources>(device))
	{
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
		impl.EquirectCS = CreateEmbeddedShader(device, "EquirectToCube.comp", nvrhi::ShaderType::Compute);
		impl.SkyCS = CreateEmbeddedShader(device, "ProceduralSky.comp", nvrhi::ShaderType::Compute);
		impl.IrradianceCS = CreateEmbeddedShader(device, "Irradiance.comp", nvrhi::ShaderType::Compute);
		impl.PrefilterCS = CreateEmbeddedShader(device, "Prefilter.comp", nvrhi::ShaderType::Compute);
		impl.BrdfCS = CreateEmbeddedShader(device, "BRDFLut.comp", nvrhi::ShaderType::Compute);
		impl.DownsampleCS = CreateEmbeddedShader(device, "CubeDownsample.comp", nvrhi::ShaderType::Compute);
		for (const nvrhi::ShaderHandle& shader : { impl.MeshVS, impl.DepthNormalPS, impl.PbrPS, impl.ShadowVS, impl.FullscreenVS, impl.SsaoPS, impl.SsaoBlurPS, impl.SkyboxVS, impl.SkyboxPS, impl.TonemapPS, impl.BlitPS, impl.EquirectCS, impl.SkyCS, impl.IrradianceCS, impl.PrefilterCS, impl.BrdfCS, impl.DownsampleCS })
		{
			if (!shader)
				return false;
		}

		// --- Input layouts ------------------------------------------------------------------------
		const nvrhi::VertexAttributeDesc meshAttributes[] = {
			nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(offsetof(Vertex, Position)).setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(offsetof(Vertex, Normal)).setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc().setName("TANGENT").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(offsetof(Vertex, Tangent)).setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(Vertex, TexCoord)).setElementStride(sizeof(Vertex)),
		};
		impl.MeshInputLayout = device->createInputLayout(meshAttributes, 4, impl.MeshVS);
		impl.ShadowInputLayout = device->createInputLayout(meshAttributes, 1, impl.ShadowVS);
		const nvrhi::VertexAttributeDesc maskedShadowAttributes[] = { meshAttributes[0], meshAttributes[3] };
		impl.ShadowMaskedInputLayout = device->createInputLayout(maskedShadowAttributes, 2, impl.ShadowMaskedVS);

		// --- Binding layouts ----------------------------------------------------------------------
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
		};
		impl.FrameLayout = device->createBindingLayout(frameDesc);

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
			nvrhi::BindingLayoutItem::PushConstants(0, 32),
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Texture_SRV(1),
			nvrhi::BindingLayoutItem::Sampler(2),
		};
		impl.BlurLayout = device->createBindingLayout(blurDesc);

		nvrhi::BindingLayoutDesc textureSamplerDesc = MakeLayout(0, nvrhi::ShaderType::Pixel);
		textureSamplerDesc.bindings = {
			nvrhi::BindingLayoutItem::PushConstants(0, 8),
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(1),
		};
		impl.TextureSamplerLayout = device->createBindingLayout(textureSamplerDesc);

		nvrhi::BindingLayoutDesc tonemapDesc = MakeLayout(0, nvrhi::ShaderType::Pixel);
		tonemapDesc.bindings = {
			nvrhi::BindingLayoutItem::PushConstants(PushConstantSlot, 16),
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(1),
			nvrhi::BindingLayoutItem::Texture_SRV(2),
			nvrhi::BindingLayoutItem::Texture_SRV(3),
			nvrhi::BindingLayoutItem::Texture_SRV(4),
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
		constantDesc.byteSize = sizeof(GpuSSAOConstants);
		constantDesc.debugName = "SSAOConstants";
		impl.SsaoConstantsBuffer = device->createBuffer(constantDesc);

		impl.DrawBuffer = EnsureStructuredBuffer(device, nullptr, sizeof(GpuDrawData) * 256, sizeof(GpuDrawData), "DrawBuffer");
		impl.MaterialBuffer = EnsureStructuredBuffer(device, nullptr, sizeof(GpuMaterialData) * 256, sizeof(GpuMaterialData), "MaterialBuffer");
		impl.LightBuffer = EnsureStructuredBuffer(device, nullptr, sizeof(GpuLightData) * MaxLights, sizeof(GpuLightData), "LightBuffer");

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

		// SSAO kernel: hemisphere samples concentrated near the origin; 4x4 rotation noise.
		std::mt19937 random(1337u);
		std::uniform_real_distribution<float> unit(0.0f, 1.0f);
		for (uint32_t i = 0; i < SSAOKernelSize; i++)
		{
			glm::vec3 sample(unit(random) * 2.0f - 1.0f, unit(random) * 2.0f - 1.0f, unit(random));
			sample = glm::normalize(sample) * unit(random);
			float scale = static_cast<float>(i) / static_cast<float>(SSAOKernelSize);
			scale = 0.1f + 0.9f * scale * scale;
			impl.SsaoConstants.Kernel[i] = glm::vec4(sample * scale, 0.0f);
		}
		std::array<uint8_t, 16 * 4> noise{};
		for (size_t i = 0; i < 16; i++)
		{
			noise[i * 4 + 0] = static_cast<uint8_t>(unit(random) * 255.0f);
			noise[i * 4 + 1] = static_cast<uint8_t>(unit(random) * 255.0f);
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

		// Shadow pipeline (fixed framebuffer layout).
		{
			nvrhi::GraphicsPipelineDesc desc;
			desc.inputLayout = impl.ShadowInputLayout;
			desc.VS = impl.ShadowVS;
			desc.renderState.rasterState.setCullNone().setDepthBias(1).setDepthClipEnable(false);
			desc.renderState.rasterState.slopeScaledDepthBias = 1.5f;
			desc.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(true).setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
			desc.bindingLayouts = { impl.FrameLayout };
			impl.ShadowPipeline = device->createGraphicsPipeline(desc, impl.ShadowFramebuffers[0]->getFramebufferInfo());

			// Alpha-tested casters (foliage, fences) discard transparent texels.
			desc.inputLayout = impl.ShadowMaskedInputLayout;
			desc.VS = impl.ShadowMaskedVS;
			desc.PS = impl.ShadowMaskedPS;
			desc.bindingLayouts = { impl.FrameLayout, impl.MaterialLayout };
			impl.ShadowMaskedPipeline = device->createGraphicsPipeline(desc, impl.ShadowFramebuffers[0]->getFramebufferInfo());
		}

		// Upload constant data.
		impl.CommandList->open();
		m_Resources->Initialize(impl.CommandList);
		impl.CommandList->writeTexture(impl.SsaoNoise, 0, 0, noise.data(), 16);
		impl.CommandList->close();
		device->executeCommandList(impl.CommandList);

		const bool valid = impl.MeshInputLayout && impl.ShadowInputLayout && impl.FrameLayout && impl.MaterialLayout && impl.SsaoLayout && impl.BlurLayout && impl.TextureSamplerLayout && impl.ComputeLayout && impl.ShadowMap && impl.ShadowPipeline && impl.ShadowMaskedPipeline && impl.ShadowMaskedInputLayout && impl.EquirectPipeline && impl.SkyPipeline && impl.IrradiancePipeline && impl.PrefilterPipeline && impl.BrdfPipeline && impl.DownsamplePipeline && impl.ViewConstantsBuffer;
		if (!valid)
			BS_CORE_ERROR("SceneRenderer: failed to create GPU resources");
		return valid;
	}

	void SceneRenderer::SetViewportSize(uint32_t width, uint32_t height)
	{
		width = std::max(width, 1u);
		height = std::max(height, 1u);
		if (width == m_Width && height == m_Height && m_OutputTexture)
			return;

		Impl& impl = *m_Impl;
		m_Device->waitForIdle();
		m_Width = width;
		m_Height = height;

		auto makeTarget = [&](nvrhi::Format format, const char* name, bool isDepth = false) {
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
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
		m_OutputTexture = makeTarget(nvrhi::Format::RGBA8_UNORM, "SceneOutput");

		impl.PrepassFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(impl.NormalTexture).setDepthAttachment(m_DepthTexture));
		impl.LightingFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(impl.HdrTexture).setDepthAttachment(m_DepthTexture));
		impl.SsaoFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(impl.SsaoTexture));
		impl.SsaoBlurFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(impl.SsaoBlurTexture));
		impl.OutputFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_OutputTexture));
		m_OverlayFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_OutputTexture).setDepthAttachment(m_DepthTexture));

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
			impl.PrepassPipeline[doubleSided] = meshPipeline(impl.DepthNormalPS, impl.PrepassFramebuffer, doubleSided, true, false);
			impl.OpaquePipeline[doubleSided] = meshPipeline(impl.PbrPS, impl.LightingFramebuffer, doubleSided, false, false);
			impl.TransparentPipeline[doubleSided] = meshPipeline(impl.PbrPS, impl.LightingFramebuffer, doubleSided, false, true);
		}

		auto fullscreenPipeline = [&](nvrhi::IShader* vertexShader, nvrhi::IShader* pixelShader, nvrhi::IBindingLayout* layout, nvrhi::IFramebuffer* framebuffer, bool depthTest) {
			nvrhi::GraphicsPipelineDesc desc;
			desc.VS = vertexShader;
			desc.PS = pixelShader;
			desc.renderState.rasterState.setCullNone();
			desc.renderState.depthStencilState.setDepthTestEnable(depthTest).setDepthWriteEnable(false).setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
			desc.bindingLayouts = { layout };
			return m_Device->createGraphicsPipeline(desc, framebuffer->getFramebufferInfo());
		};
		impl.SsaoPipeline = fullscreenPipeline(impl.FullscreenVS, impl.SsaoPS, impl.SsaoLayout, impl.SsaoFramebuffer, false);
		impl.SsaoBlurPipeline = fullscreenPipeline(impl.FullscreenVS, impl.SsaoBlurPS, impl.BlurLayout, impl.SsaoBlurFramebuffer, false);
		impl.TonemapPipeline = fullscreenPipeline(impl.FullscreenVS, impl.TonemapPS, impl.TonemapLayout, impl.OutputFramebuffer, false);
		impl.SkyboxPipeline = fullscreenPipeline(impl.SkyboxVS, impl.SkyboxPS, impl.FrameLayout, impl.LightingFramebuffer, true);

		impl.SsaoSet = m_Device->createBindingSet(nvrhi::BindingSetDesc().setTrackLiveness(true).addItem(nvrhi::BindingSetItem::ConstantBuffer(0, impl.SsaoConstantsBuffer)).addItem(nvrhi::BindingSetItem::Texture_SRV(1, m_DepthTexture)).addItem(nvrhi::BindingSetItem::Texture_SRV(2, impl.NormalTexture)).addItem(nvrhi::BindingSetItem::Texture_SRV(3, impl.SsaoNoise)).addItem(nvrhi::BindingSetItem::Sampler(4, impl.PointClamp)).addItem(nvrhi::BindingSetItem::Sampler(5, impl.PointWrap)), impl.SsaoLayout);
		impl.BlurHorizontalSet = m_Device->createBindingSet(nvrhi::BindingSetDesc()
																.addItem(nvrhi::BindingSetItem::PushConstants(0, 32))
																.addItem(nvrhi::BindingSetItem::Texture_SRV(0, impl.SsaoTexture))
																.addItem(nvrhi::BindingSetItem::Texture_SRV(1, m_DepthTexture))
																.addItem(nvrhi::BindingSetItem::Sampler(2, impl.PointClamp)),
															impl.BlurLayout);
		impl.BlurVerticalSet = m_Device->createBindingSet(nvrhi::BindingSetDesc()
															  .addItem(nvrhi::BindingSetItem::PushConstants(0, 32))
															  .addItem(nvrhi::BindingSetItem::Texture_SRV(0, impl.SsaoBlurTexture))
															  .addItem(nvrhi::BindingSetItem::Texture_SRV(1, m_DepthTexture))
															  .addItem(nvrhi::BindingSetItem::Sampler(2, impl.PointClamp)),
														  impl.BlurLayout);
		impl.TonemapSet = m_Device->createBindingSet(nvrhi::BindingSetDesc()
														 .addItem(nvrhi::BindingSetItem::PushConstants(PushConstantSlot, 16))
														 .addItem(nvrhi::BindingSetItem::Texture_SRV(0, impl.HdrTexture))
														 .addItem(nvrhi::BindingSetItem::Sampler(1, impl.PointClamp))
														 .addItem(nvrhi::BindingSetItem::Texture_SRV(2, impl.SsaoTexture))
														 .addItem(nvrhi::BindingSetItem::Texture_SRV(3, impl.NormalTexture))
														 .addItem(nvrhi::BindingSetItem::Texture_SRV(4, m_DepthTexture)),
													 impl.TonemapLayout);
		impl.BlitSet = m_Device->createBindingSet(nvrhi::BindingSetDesc()
													  .addItem(nvrhi::BindingSetItem::PushConstants(0, 8))
													  .addItem(nvrhi::BindingSetItem::Texture_SRV(0, m_OutputTexture))
													  .addItem(nvrhi::BindingSetItem::Sampler(1, impl.LinearClamp)),
												  impl.TextureSamplerLayout);
		impl.BlitPipelines.clear();
		impl.FrameSetDirty = true;
	}

	void SceneRenderer::Render(Scene& scene, const RenderCamera& camera)
	{
		Impl& impl = *m_Impl;
		if (!m_OutputTexture)
			SetViewportSize(std::max(m_Width, 1u), std::max(m_Height, 1u));

		nvrhi::ICommandList* commandList = impl.CommandList;
		commandList->open();
		impl.FrameCounter++;
		m_Statistics = {};
		const RendererSettings& settings = scene.GetRendererSettings();

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

		// --- Lights -------------------------------------------------------------------------------
		GpuDirectionalLight sun{};
		sun.Direction = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
		{
			// One directional light (the first) is supported.
			auto view = scene.GetAllEntitiesWith<DirectionalLightComponent>();
			if (auto it = view.begin(); it != view.end())
			{
				const entt::entity handle = *it;
				Entity entity(handle, &scene);
				const auto& light = view.get<DirectionalLightComponent>(handle);
				const glm::mat4 world = scene.GetWorldTransform(entity);
				const glm::vec3 direction = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
				sun.Direction = glm::vec4(direction, 1.0f);
				sun.Color = glm::vec4(light.Color * light.Intensity, (light.CastShadows && settings.ShadowsEnabled) ? 1.0f : 0.0f);
				sun.ShadowParams = glm::vec4(settings.ShadowBias, settings.ShadowNormalBias, light.ShadowSoftness, static_cast<float>(MaxCascades));
				m_Statistics.Lights++;
			}
		}

		std::vector<GpuLightData>& lights = impl.Lights;
		lights.clear();
		{
			auto pointView = scene.GetAllEntitiesWith<PointLightComponent>();
			for (entt::entity handle : pointView)
			{
				if (lights.size() >= MaxLights)
					break;
				const auto& light = pointView.get<PointLightComponent>(handle);
				const glm::vec3 position(scene.GetWorldTransform({ handle, &scene })[3]);
				lights.push_back({ glm::vec4(position, light.Radius), glm::vec4(light.Color * light.Intensity, 0.0f), glm::vec4(0.0f), glm::vec4(0.0f) });
			}
			auto spotView = scene.GetAllEntitiesWith<SpotLightComponent>();
			for (entt::entity handle : spotView)
			{
				if (lights.size() >= MaxLights)
					break;
				const auto& light = spotView.get<SpotLightComponent>(handle);
				const glm::mat4 world = scene.GetWorldTransform({ handle, &scene });
				const glm::vec3 direction = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
				const float outerDegrees = std::clamp(light.OuterConeAngle, 0.1f, 89.9f);
				const float outer = glm::cos(glm::radians(outerDegrees));
				const float inner = glm::cos(glm::radians(std::clamp(light.InnerConeAngle, 0.0f, outerDegrees)));
				lights.push_back({ glm::vec4(glm::vec3(world[3]), light.Range), glm::vec4(light.Color * light.Intensity, 1.0f), glm::vec4(direction, outer), glm::vec4(std::max(inner, outer + 1e-4f), 0.0f, 0.0f, 0.0f) });
			}
		}
		m_Statistics.Lights += static_cast<uint32_t>(lights.size());

		// --- Environment --------------------------------------------------------------------------
		const SkyLightComponent* skyLight = nullptr;
		{
			auto view = scene.GetAllEntitiesWith<SkyLightComponent>();
			if (auto it = view.begin(); it != view.end())
				skyLight = &view.get<SkyLightComponent>(*it);
		}
		const std::string environmentKey = skyLight ? skyLight->EnvironmentMap : std::string();
		Ref<TextureSource> environmentSource = environmentKey.empty() ? nullptr : AssetManager::GetTexture(environmentKey);
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
		if (bakeKey != impl.EnvironmentKey || !impl.Env.Cube)
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

		// --- Camera, cascades and view constants --------------------------------------------------
		GpuViewConstants view{};
		view.View = camera.View;
		view.Projection = camera.Projection;
		view.ViewProjection = camera.Projection * camera.View;
		view.InverseView = glm::inverse(camera.View);
		view.InverseProjection = glm::inverse(camera.Projection);
		view.CameraPosition = glm::vec4(camera.Position, 1.0f);
		view.ViewportSize = glm::vec4(static_cast<float>(m_Width), static_cast<float>(m_Height), 1.0f / static_cast<float>(m_Width), 1.0f / static_cast<float>(m_Height));
		view.CameraClip = glm::vec4(camera.Near, camera.Far, 0.0f, 0.0f);
		const float iblIntensity = skyLight ? skyLight->Intensity : 1.0f;
		view.AmbientColor = glm::vec4(settings.AmbientColor, iblIntensity);
		view.EnvironmentParams = glm::vec4(skyLight ? glm::radians(skyLight->Rotation) : 0.0f, static_cast<float>(PrefilteredMips), settings.SSAOEnabled ? 1.0f : 0.0f, static_cast<float>(lights.size()));
		view.Sun = sun;

		const bool shadowsActive = sun.Direction.w > 0.5f && sun.Color.w > 0.5f;
		if (shadowsActive)
		{
			// Split the camera frustum (up to ShadowDistance) into cascades.
			const float nearClip = camera.Near;
			const float farClip = std::min(camera.Far, std::max(settings.ShadowDistance, nearClip + 1.0f));
			const auto splits = ComputeCascadeSplits<MaxCascades>(nearClip, farClip, settings.CascadeSplitLambda, IsOrthographic(camera.Projection));
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
			const glm::vec3 up = glm::abs(lightDirection.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
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

		commandList->writeBuffer(impl.ViewConstantsBuffer, &view, sizeof(view));

		// --- Draw list ----------------------------------------------------------------------------
		const Frustum frustum(view.ViewProjection);
		std::vector<Frustum> cascadeFrusta;
		if (shadowsActive)
		{
			for (uint32_t cascade = 0; cascade < MaxCascades; cascade++)
				cascadeFrusta.emplace_back(view.CascadeViewProjection[cascade]);
		}
		std::vector<GpuDrawData>& draws = impl.Draws;
		std::vector<GpuMaterialData>& materials = impl.Materials;
		std::vector<DrawItem>& items = impl.Items;
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

		auto texture = [&](const std::string& key, bool srgb, nvrhi::ITexture* fallback, uint32_t flag, uint32_t& flags) -> nvrhi::ITexture* {
			if (key.empty())
				return fallback;
			Ref<TextureSource> source = AssetManager::GetTexture(key);
			nvrhi::ITexture* gpu = source ? m_Resources->GetTexture(source, srgb, commandList) : nullptr;
			if (!gpu)
				return fallback;
			flags |= flag;
			return gpu;
		};

		const glm::vec3 cameraForward = -glm::vec3(view.InverseView[2]);
		auto meshView = scene.GetAllEntitiesWith<MeshComponent>();
		for (entt::entity handle : meshView)
		{
			Entity entity(handle, &scene);
			const auto& meshComponent = meshView.get<MeshComponent>(handle);
			Ref<MeshSource> source = AssetManager::GetMesh(meshComponent.Mesh);
			if (!source || meshComponent.MeshIndex >= source->Meshes.size())
				continue;
			const GpuMesh* gpuMesh = m_Resources->GetMesh(source, commandList);
			if (!gpuMesh)
				continue;

			const glm::mat4 model = scene.GetWorldTransform(entity);
			const glm::mat4 normalMatrix = glm::transpose(glm::inverse(model));
			const auto* materialOverride = entity.TryGetComponent<MaterialComponent>();

			for (uint32_t submeshIndex : source->Meshes[meshComponent.MeshIndex].Submeshes)
			{
				const Submesh& submesh = source->Submeshes[submeshIndex];
				const AABB worldBounds = submesh.Bounds.Transformed(model);
				const bool visible = frustum.Intersects(worldBounds);
				const bool castsShadow = meshComponent.CastShadows && std::any_of(cascadeFrusta.begin(), cascadeFrusta.end(), [&](const Frustum& cascade) { return cascade.Intersects(worldBounds); });
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

				const uint32_t materialIndex = static_cast<uint32_t>(materials.size());
				materials.push_back(material);
				const uint32_t drawIndex = static_cast<uint32_t>(draws.size());
				draws.push_back({ model, normalMatrix, glm::uvec4(materialIndex, 0, 0, 0) });

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
		}
		m_Statistics.Materials = static_cast<uint32_t>(materials.size());

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
		impl.MaterialBuffer = EnsureStructuredBuffer(m_Device, impl.MaterialBuffer, materials.size() * sizeof(GpuMaterialData), sizeof(GpuMaterialData), "MaterialBuffer");
		if (impl.DrawBuffer != previousDraws || impl.MaterialBuffer != previousMaterials)
			impl.FrameSetDirty = true;
		commandList->writeBuffer(impl.DrawBuffer, draws.data(), draws.size() * sizeof(GpuDrawData));
		commandList->writeBuffer(impl.MaterialBuffer, materials.data(), materials.size() * sizeof(GpuMaterialData));
		commandList->writeBuffer(impl.LightBuffer, lights.data(), lights.size() * sizeof(GpuLightData));

		if (impl.FrameSetDirty)
		{
			impl.FrameSet = m_Device->createBindingSet(nvrhi::BindingSetDesc()
														   .addItem(nvrhi::BindingSetItem::PushConstants(PushConstantSlot, sizeof(DrawPush)))
														   .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, impl.ViewConstantsBuffer))
														   .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, impl.DrawBuffer))
														   .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, impl.MaterialBuffer))
														   .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, impl.LightBuffer))
														   .addItem(nvrhi::BindingSetItem::Texture_SRV(4, impl.ShadowMap, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::Texture2DArray))
														   .addItem(nvrhi::BindingSetItem::Texture_SRV(5, impl.Env.Irradiance, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::TextureCube))
														   .addItem(nvrhi::BindingSetItem::Texture_SRV(6, impl.Env.Prefiltered, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::TextureCube))
														   .addItem(nvrhi::BindingSetItem::Texture_SRV(7, impl.BrdfLut))
														   .addItem(nvrhi::BindingSetItem::Texture_SRV(8, impl.SsaoTexture))
														   .addItem(nvrhi::BindingSetItem::Sampler(9, impl.LinearClamp))
														   .addItem(nvrhi::BindingSetItem::Sampler(10, impl.ShadowCompare))
														   .addItem(nvrhi::BindingSetItem::Sampler(11, impl.PointClamp))
														   .addItem(nvrhi::BindingSetItem::Texture_SRV(12, impl.Env.Cube, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::TextureCube)),
													   impl.FrameLayout);
			impl.FrameSetDirty = false;
		}

		auto drawItem = [&](const DrawItem& item, nvrhi::IGraphicsPipeline* pipeline, nvrhi::IFramebuffer* framebuffer, uint32_t cascade, bool withMaterial) {
			nvrhi::GraphicsState state;
			state.pipeline = pipeline;
			state.framebuffer = framebuffer;
			state.bindings = { impl.FrameSet };
			if (withMaterial)
				state.bindings.push_back(item.MaterialSet);
			state.vertexBuffers = { nvrhi::VertexBufferBinding().setBuffer(item.Mesh->VertexBuffer).setSlot(0).setOffset(0) };
			state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(item.Mesh->IndexBuffer).setFormat(nvrhi::Format::R32_UINT).setOffset(0);
			const nvrhi::FramebufferInfoEx& info = framebuffer->getFramebufferInfo();
			state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(info.width), static_cast<float>(info.height)));
			commandList->setGraphicsState(state);
			const DrawPush push{ item.DrawIndex, cascade };
			commandList->setPushConstants(&push, sizeof(push));
			nvrhi::DrawArguments args;
			args.vertexCount = item.IndexCount;
			args.startIndexLocation = item.BaseIndex;
			args.startVertexLocation = item.BaseVertex;
			commandList->drawIndexed(args);
		};

		// --- Shadow maps --------------------------------------------------------------------------
		commandList->beginMarker("Shadows");
		commandList->clearDepthStencilTexture(impl.ShadowMap, nvrhi::AllSubresources, true, 1.0f, false, 0);
		if (shadowsActive)
		{
			for (uint32_t cascade = 0; cascade < MaxCascades; cascade++)
			{
				for (const DrawItem& item : items)
				{
					if (!item.CastShadows || !cascadeFrusta[cascade].Intersects(item.ShadowBounds))
						continue;
					if (item.AlphaTested)
						drawItem(item, impl.ShadowMaskedPipeline, impl.ShadowFramebuffers[cascade], cascade, true);
					else
						drawItem(item, impl.ShadowPipeline, impl.ShadowFramebuffers[cascade], cascade, false);
					m_Statistics.ShadowDrawCalls++;
				}
			}
		}
		commandList->endMarker();

		// --- Depth/normal prepass (opaque, visible) -----------------------------------------------
		commandList->beginMarker("Prepass");
		commandList->clearDepthStencilTexture(m_DepthTexture, nvrhi::AllSubresources, true, 1.0f, false, 0);
		commandList->clearTextureFloat(impl.NormalTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 1.0f, 0.0f));
		for (const DrawItem& item : items)
		{
			if (item.Blend || !item.WorldBounds.IsValid())
				continue;
			drawItem(item, impl.PrepassPipeline[item.DoubleSided ? 1 : 0], impl.PrepassFramebuffer, 0, true);
		}
		commandList->endMarker();

		// --- SSAO ---------------------------------------------------------------------------------
		if (settings.SSAOEnabled)
		{
			commandList->beginMarker("SSAO");
			impl.SsaoConstants.Projection = camera.Projection;
			impl.SsaoConstants.InverseProjection = view.InverseProjection;
			impl.SsaoConstants.Params = glm::vec4(settings.SSAORadius, settings.SSAOBias, settings.SSAOIntensity, static_cast<float>(SSAOKernelSize));
			impl.SsaoConstants.ScreenSize = glm::vec4(static_cast<float>(m_Width), static_cast<float>(m_Height), static_cast<float>(m_Width) / 4.0f, static_cast<float>(m_Height) / 4.0f);
			commandList->writeBuffer(impl.SsaoConstantsBuffer, &impl.SsaoConstants, sizeof(GpuSSAOConstants));

			auto fullscreen = [&](nvrhi::IGraphicsPipeline* pipeline, nvrhi::IFramebuffer* framebuffer, nvrhi::IBindingSet* set, const void* push, size_t pushSize) {
				nvrhi::GraphicsState state;
				state.pipeline = pipeline;
				state.framebuffer = framebuffer;
				state.bindings = { set };
				state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_Width), static_cast<float>(m_Height)));
				commandList->setGraphicsState(state);
				if (push)
					commandList->setPushConstants(push, pushSize);
				commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
			};
			fullscreen(impl.SsaoPipeline, impl.SsaoFramebuffer, impl.SsaoSet, nullptr, 0);
			const float orthographic = IsOrthographic(camera.Projection) ? 1.0f : 0.0f;
			const float horizontal[8] = { 1.0f / static_cast<float>(m_Width), 0.0f, camera.Near, camera.Far, orthographic, 0.0f, 0.0f, 0.0f };
			fullscreen(impl.SsaoBlurPipeline, impl.SsaoBlurFramebuffer, impl.BlurHorizontalSet, horizontal, sizeof(horizontal));
			const float vertical[8] = { 0.0f, 1.0f / static_cast<float>(m_Height), camera.Near, camera.Far, orthographic, 0.0f, 0.0f, 0.0f };
			fullscreen(impl.SsaoBlurPipeline, impl.SsaoFramebuffer, impl.BlurVerticalSet, vertical, sizeof(vertical));
			commandList->endMarker();
		}

		// --- Lighting (opaque), skybox, transparent -----------------------------------------------
		commandList->beginMarker("Lighting");
		commandList->clearTextureFloat(impl.HdrTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
		for (const DrawItem& item : items)
		{
			if (item.Blend || !item.WorldBounds.IsValid())
				continue;
			drawItem(item, impl.OpaquePipeline[item.DoubleSided ? 1 : 0], impl.LightingFramebuffer, 0, true);
			m_Statistics.DrawCalls++;
		}

		if (!skyLight || skyLight->ShowBackground)
		{
			nvrhi::GraphicsState state;
			state.pipeline = impl.SkyboxPipeline;
			state.framebuffer = impl.LightingFramebuffer;
			state.bindings = { impl.FrameSet };
			state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_Width), static_cast<float>(m_Height)));
			commandList->setGraphicsState(state);
			const DrawPush push{ 0, 0 };
			commandList->setPushConstants(&push, sizeof(push));
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		}

		std::vector<const DrawItem*>& transparent = impl.Transparent;
		transparent.clear();
		for (const DrawItem& item : items)
		{
			if (item.Blend && item.WorldBounds.IsValid())
				transparent.push_back(&item);
		}
		std::sort(transparent.begin(), transparent.end(), [](const DrawItem* a, const DrawItem* b) { return a->ViewDepth > b->ViewDepth; });
		for (const DrawItem* item : transparent)
		{
			drawItem(*item, impl.TransparentPipeline[item->DoubleSided ? 1 : 0], impl.LightingFramebuffer, 0, true);
			m_Statistics.DrawCalls++;
		}
		commandList->endMarker();

		// --- Tonemap ------------------------------------------------------------------------------
		{
			commandList->beginMarker("Tonemap");
			nvrhi::GraphicsState state;
			state.pipeline = impl.TonemapPipeline;
			state.framebuffer = impl.OutputFramebuffer;
			state.bindings = { impl.TonemapSet };
			state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_Width), static_cast<float>(m_Height)));
			commandList->setGraphicsState(state);
			const struct
			{
				float Exposure;
				uint32_t Operator;
				uint32_t DebugView;
				float Padding;
			} push = { settings.Exposure, static_cast<uint32_t>(settings.Tonemap), static_cast<uint32_t>(settings.DebugView), 0.0f };
			commandList->setPushConstants(&push, sizeof(push));
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
			commandList->endMarker();
		}

		// --- Debug lines (scripts, editor gizmos) --------------------------------------------------
		const std::vector<DebugLineVertex> lines = DebugDraw::TakeLines();
		if (!lines.empty())
		{
			commandList->beginMarker("DebugLines");
			impl.Lines.Render(commandList, m_OverlayFramebuffer, view.ViewProjection, lines);
			commandList->endMarker();
		}

		commandList->close();
		m_Device->executeCommandList(commandList);

		// Release cached material sets (and the textures they reference) that are no longer used.
		std::erase_if(impl.MaterialSets, [&](const auto& entry) { return impl.FrameCounter - entry.second.LastUsed > MaterialSetRetentionFrames; });
		if (impl.FrameCounter % 300 == 0)
			m_Resources->CollectGarbage();
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

		nvrhi::GraphicsState state;
		state.pipeline = it->second;
		state.framebuffer = target;
		state.bindings = { impl.BlitSet };
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(info.width), static_cast<float>(info.height)));
		commandList->setGraphicsState(state);
		const uint32_t unused[2] = {};
		commandList->setPushConstants(unused, sizeof(unused));
		commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
	}

	bool SceneRenderer::ReadOutputPixels(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight)
	{
		if (!m_OutputTexture)
			return false;

		nvrhi::TextureDesc desc = m_OutputTexture->getDesc();
		desc.isRenderTarget = false;
		desc.debugName = "OutputReadback";
		desc.initialState = nvrhi::ResourceStates::CopyDest;
		desc.keepInitialState = true;
		nvrhi::StagingTextureHandle staging = m_Device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
		if (!staging)
			return false;

		nvrhi::CommandListHandle commandList = m_Device->createCommandList();
		commandList->open();
		commandList->copyTexture(staging, nvrhi::TextureSlice(), m_OutputTexture, nvrhi::TextureSlice());
		commandList->close();
		m_Device->executeCommandList(commandList);
		m_Device->waitForIdle();

		size_t rowPitch = 0;
		const auto* data = static_cast<const uint8_t*>(m_Device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
		if (!data)
			return false;

		outWidth = desc.width;
		outHeight = desc.height;
		outPixels.resize(static_cast<size_t>(outWidth) * outHeight * 4);
		for (uint32_t y = 0; y < outHeight; y++)
			std::memcpy(outPixels.data() + static_cast<size_t>(y) * outWidth * 4, data + y * rowPitch, static_cast<size_t>(outWidth) * 4);
		m_Device->unmapStagingTexture(staging);
		return true;
	}

}
