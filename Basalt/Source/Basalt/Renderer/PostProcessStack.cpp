#include "Basalt/Renderer/PostProcessStack.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Renderer/ShaderUtils.h"
#include "Basalt/Scene/Components.h"

#include <algorithm>
#include <cmath>

namespace Basalt {
	namespace {
		struct BloomPush
		{
			glm::vec2 SourceSize, BaseSize;
			float Threshold, Scatter;
			uint32_t Mode;
		};
		struct ExposurePush
		{
			glm::uvec2 Size;
			float MinExposure, MaxExposure, Compensation, Blend;
			uint32_t Reset;
		};
		struct CompositePush
		{
			glm::vec2 BloomSize;
			float BloomIntensity;
			uint32_t AutoExposure;
		};
		struct FxaaPush
		{
			glm::vec2 RenderSize;
			uint32_t Dither;
		};
		static_assert(sizeof(BloomPush) == 28 && sizeof(ExposurePush) == 28 && sizeof(CompositePush) == 16 && sizeof(FxaaPush) == 12);
		glm::uvec2 Half(glm::uvec2 size)
		{
			return glm::max((size + 1u) / 2u, glm::uvec2(1));
		}
	}

	bool PostProcessStack::Fail(const char* stage)
	{
		BS_CORE_ERROR("SceneRenderer: failed to create post-process {}", stage);
		m_Failed = true;
		return false;
	}

	bool PostProcessStack::Prepare(nvrhi::IDevice* device, nvrhi::ITexture* hdr, nvrhi::IShader* fullscreen, nvrhi::ISampler* sampler)
	{
		if (m_Source != hdr || m_Device != device || m_Fullscreen != fullscreen || m_Sampler != sampler)
		{
			Reset();
			m_Device = device;
			m_Source = hdr;
			m_Fullscreen = fullscreen;
			m_Sampler = sampler;
		}
		if (m_Failed)
			return false;
		if (m_CompositeSet)
			return true;
		auto target = [&](glm::uvec2 size, const char* name, nvrhi::Format format = nvrhi::Format::RGBA16_FLOAT) {
			nvrhi::TextureDesc desc;
			desc.width = size.x;
			desc.height = size.y;
			desc.format = format;
			desc.isRenderTarget = format != nvrhi::Format::R32_FLOAT;
			desc.isUAV = format == nvrhi::Format::R32_FLOAT;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = name;
			return device->createTexture(desc);
		};
		glm::uvec2 size{ hdr->getDesc().width, hdr->getDesc().height };
		m_Composite = target(size, "PostProcessHDR");
		m_Exposure = target({ 1, 1 }, "AutoExposure", nvrhi::Format::R32_FLOAT);
		if (!m_Composite || !m_Exposure)
			return Fail("HDR/exposure textures");
		m_CompositeFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_Composite));
		if (!m_CompositeFramebuffer)
			return Fail("HDR framebuffer");
		auto bloomDesc = MakeLayout(0, nvrhi::ShaderType::Pixel);
		bloomDesc.bindings = { nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(1), nvrhi::BindingLayoutItem::Texture_SRV(2), nvrhi::BindingLayoutItem::PushConstants(PushConstantSlot, sizeof(BloomPush)) };
		m_BloomLayout = device->createBindingLayout(bloomDesc);
		auto compositeDesc = MakeLayout(0, nvrhi::ShaderType::Pixel);
		compositeDesc.bindings = { nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(1), nvrhi::BindingLayoutItem::Texture_SRV(2), nvrhi::BindingLayoutItem::Texture_SRV(3), nvrhi::BindingLayoutItem::PushConstants(PushConstantSlot, sizeof(CompositePush)) };
		m_CompositeLayout = device->createBindingLayout(compositeDesc);
		auto exposureDesc = MakeLayout(0, nvrhi::ShaderType::Compute);
		exposureDesc.bindings = { nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(1), nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2), nvrhi::BindingLayoutItem::Texture_UAV(3), nvrhi::BindingLayoutItem::PushConstants(PushConstantSlot, sizeof(ExposurePush)) };
		m_ExposureLayout = device->createBindingLayout(exposureDesc);
		if (!m_BloomLayout || !m_CompositeLayout || !m_ExposureLayout)
			return Fail("layouts");
		const auto bloomPS = CreateEmbeddedShader(device, "Bloom.frag", nvrhi::ShaderType::Pixel);
		const auto compositePS = CreateEmbeddedShader(device, "PostProcessComposite.frag", nvrhi::ShaderType::Pixel);
		const auto histogramCS = CreateEmbeddedShader(device, "ExposureHistogram.comp", nvrhi::ShaderType::Compute);
		const auto reduceCS = CreateEmbeddedShader(device, "ExposureReduce.comp", nvrhi::ShaderType::Compute);
		if (!bloomPS || !compositePS || !histogramCS || !reduceCS)
			return Fail("shaders");
		m_BloomPipeline = CreateFullscreenPipeline(device, fullscreen, bloomPS, m_BloomLayout, m_CompositeFramebuffer);
		m_CompositePipeline = CreateFullscreenPipeline(device, fullscreen, compositePS, m_CompositeLayout, m_CompositeFramebuffer);
		m_HistogramPipeline = device->createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(histogramCS).addBindingLayout(m_ExposureLayout));
		m_ReducePipeline = device->createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(reduceCS).addBindingLayout(m_ExposureLayout));
		if (!m_BloomPipeline || !m_CompositePipeline || !m_HistogramPipeline || !m_ReducePipeline)
			return Fail("pipelines");
		nvrhi::BufferDesc buffer;
		buffer.byteSize = 256 * sizeof(uint32_t);
		buffer.structStride = sizeof(uint32_t);
		buffer.canHaveUAVs = true;
		buffer.initialState = nvrhi::ResourceStates::UnorderedAccess;
		buffer.keepInitialState = true;
		buffer.debugName = "ExposureHistogram";
		m_Histogram = device->createBuffer(buffer);
		if (!m_Histogram)
			return Fail("histogram buffer");
		m_ExposureSet = device->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::Texture_SRV(0, hdr)).addItem(nvrhi::BindingSetItem::Sampler(1, sampler)).addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(2, m_Histogram)).addItem(nvrhi::BindingSetItem::Texture_UAV(3, m_Exposure)).addItem(nvrhi::BindingSetItem::PushConstants(PushConstantSlot, sizeof(ExposurePush))), m_ExposureLayout);
		if (!m_ExposureSet)
			return Fail("exposure bindings");
		// Separate images avoid read/write aliasing on an upsample; levels retain allocations during dynamic scaling.
		for (uint32_t i = 0; i < 6; ++i)
		{
			size = Half(size);
			BloomLevel level;
			level.Down = target(size, "BloomDown");
			level.Up = target(size, "BloomUp");
			if (!level.Down || !level.Up)
				return Fail("bloom textures");
			level.DownFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(level.Down));
			level.UpFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(level.Up));
			if (!level.DownFramebuffer || !level.UpFramebuffer)
				return Fail("bloom framebuffers");
			m_Bloom.push_back(std::move(level));
			if (size == glm::uvec2(1))
				break;
		}
		auto bloomSet = [&](nvrhi::ITexture* source, nvrhi::ITexture* base) {
			return device->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::Texture_SRV(0, source)).addItem(nvrhi::BindingSetItem::Sampler(1, sampler)).addItem(nvrhi::BindingSetItem::Texture_SRV(2, base)).addItem(nvrhi::BindingSetItem::PushConstants(PushConstantSlot, sizeof(BloomPush))), m_BloomLayout);
		};
		for (size_t i = 0; i < m_Bloom.size(); ++i)
		{
			auto& level = m_Bloom[i];
			level.DownSet = bloomSet(i == 0 ? hdr : m_Bloom[i - 1].Down.Get(), hdr);
			level.UpSet = bloomSet(i + 1 < m_Bloom.size() ? m_Bloom[i + 1].Up.Get() : level.Down.Get(), level.Down);
			if (!level.DownSet || !level.UpSet)
				return Fail("bloom bindings");
		}
		m_CompositeSet = device->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::Texture_SRV(0, hdr)).addItem(nvrhi::BindingSetItem::Sampler(1, sampler)).addItem(nvrhi::BindingSetItem::Texture_SRV(2, m_Bloom.front().Up)).addItem(nvrhi::BindingSetItem::Texture_SRV(3, m_Exposure)).addItem(nvrhi::BindingSetItem::PushConstants(PushConstantSlot, sizeof(CompositePush))), m_CompositeLayout);
		return m_CompositeSet ? true : Fail("composite bindings");
	}

	void PostProcessStack::RenderHDR(nvrhi::ICommandList* commands, glm::uvec2 size, const PostProcessComponent& settings, float deltaTime, bool resetExposure)
	{
		if (settings.AutoExposure)
		{
			commands->clearBufferUInt(m_Histogram, 0);
			const float dt = std::isfinite(deltaTime) ? std::clamp(deltaTime, 0.0f, 1.0f) : 0.0f;
			const ExposurePush push{ size, settings.MinExposure, settings.MaxExposure, settings.ExposureCompensation, 1.0f - std::exp(-settings.AdaptationSpeed * dt), resetExposure || !m_AutoLastFrame ? 1u : 0u };
			nvrhi::ComputeState state;
			state.pipeline = m_HistogramPipeline;
			state.bindings = { m_ExposureSet };
			commands->setComputeState(state);
			commands->setPushConstants(&push, sizeof(push));
			commands->dispatch((size.x + 15) / 16, (size.y + 15) / 16, 1);
			// nvrhi skips automatic barriers when only the pipeline changes with identical bindings.
			commands->setBufferState(m_Histogram, nvrhi::ResourceStates::UnorderedAccess);
			commands->commitBarriers();
			state.pipeline = m_ReducePipeline;
			commands->setComputeState(state);
			commands->setPushConstants(&push, sizeof(push));
			commands->dispatch(1, 1, 1);
		}
		else
			commands->clearTextureFloat(m_Exposure, nvrhi::AllSubresources, nvrhi::Color(1));
		m_AutoLastFrame = settings.AutoExposure;
		std::vector<glm::uvec2> sizes;
		glm::uvec2 sourceSize = size;
		for (size_t i = 0; i < m_Bloom.size(); ++i)
		{
			const glm::uvec2 levelSize = Half(sourceSize);
			sizes.push_back(levelSize);
			if (settings.BloomEnabled)
			{
				const BloomPush push{ glm::vec2(sourceSize), {}, settings.BloomThreshold, settings.BloomScatter, i == 0 ? 0u : 1u };
				DrawFullscreen(commands, m_BloomPipeline, m_Bloom[i].DownFramebuffer, m_Bloom[i].DownSet, nvrhi::Viewport(static_cast<float>(levelSize.x), static_cast<float>(levelSize.y)), &push, sizeof(push));
			}
			sourceSize = levelSize;
		}
		if (settings.BloomEnabled)
		{
			for (size_t i = m_Bloom.size(); i-- > 0;)
			{
				const auto source = i + 1 < sizes.size() ? sizes[i + 1] : sizes[i];
				const BloomPush push{ glm::vec2(source), glm::vec2(sizes[i]), 0, settings.BloomScatter, 2 };
				DrawFullscreen(commands, m_BloomPipeline, m_Bloom[i].UpFramebuffer, m_Bloom[i].UpSet, nvrhi::Viewport(static_cast<float>(sizes[i].x), static_cast<float>(sizes[i].y)), &push, sizeof(push));
			}
		}
		else
			commands->clearTextureFloat(m_Bloom.front().Up, nvrhi::AllSubresources, nvrhi::Color(0));
		const CompositePush push{ glm::vec2(sizes.front()), settings.BloomEnabled ? settings.BloomIntensity : 0.0f, settings.AutoExposure ? 1u : 0u };
		DrawFullscreen(commands, m_CompositePipeline, m_CompositeFramebuffer, m_CompositeSet, nvrhi::Viewport(static_cast<float>(size.x), static_cast<float>(size.y)), &push, sizeof(push));
	}

	bool PostProcessStack::PrepareFXAA(nvrhi::IFramebuffer* output)
	{
		if (m_Failed)
			return false;
		if (!m_FxaaInput)
		{
			auto desc = m_Composite->getDesc();
			desc.debugName = "FXAAInput";
			const auto texture = m_Device->createTexture(desc);
			if (!texture)
				return Fail("FXAA texture");
			m_FxaaInput = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));
			if (!m_FxaaInput)
				return Fail("FXAA framebuffer");
			auto layout = MakeLayout(0, nvrhi::ShaderType::Pixel);
			layout.bindings = { nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(1), nvrhi::BindingLayoutItem::PushConstants(PushConstantSlot, sizeof(FxaaPush)) };
			m_FxaaLayout = m_Device->createBindingLayout(layout);
			if (!m_FxaaLayout)
				return Fail("FXAA layout");
			m_FxaaSet = m_Device->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture)).addItem(nvrhi::BindingSetItem::Sampler(1, m_Sampler)).addItem(nvrhi::BindingSetItem::PushConstants(PushConstantSlot, sizeof(FxaaPush))), m_FxaaLayout);
			if (!m_FxaaSet)
				return Fail("FXAA bindings");
		}
		const auto format = output->getFramebufferInfo().colorFormats[0];
		if (m_FxaaPipeline && m_FxaaFormat == format)
			return true;
		m_FxaaFormat = format;
		// Output can switch between the UNORM final image and FSR's float input.
		const auto shader = CreateEmbeddedShader(m_Device, "FXAA.frag", nvrhi::ShaderType::Pixel);
		if (!shader)
			return Fail("FXAA shader");
		m_FxaaPipeline = CreateFullscreenPipeline(m_Device, m_Fullscreen, shader, m_FxaaLayout, output);
		return m_FxaaPipeline ? true : Fail("FXAA pipeline");
	}

	void PostProcessStack::RenderFXAA(nvrhi::ICommandList* commands, nvrhi::IFramebuffer* output, glm::uvec2 size, bool finalOutput)
	{
		const FxaaPush push{ glm::vec2(size), finalOutput ? 1u : 0u };
		const auto viewport = finalOutput ? output->getFramebufferInfo().getViewport() : nvrhi::Viewport(static_cast<float>(size.x), static_cast<float>(size.y));
		DrawFullscreen(commands, m_FxaaPipeline, output, m_FxaaSet, viewport, &push, sizeof(push));
	}
}
