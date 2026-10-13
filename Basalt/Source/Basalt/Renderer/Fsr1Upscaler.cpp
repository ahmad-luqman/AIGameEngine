#include "Basalt/Renderer/Fsr1Upscaler.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Renderer/ShaderUtils.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Basalt {

	namespace {
		// Matches FsrPush in both shaders. Dimensions are exact integers represented as floats.
		struct FsrPush
		{
			glm::vec2 RenderSize;
			glm::vec2 OutputSize;
			float Sharpness;
		};
		static_assert(sizeof(FsrPush) == 20 && offsetof(FsrPush, Sharpness) == 16);
	} // namespace

	void Fsr1Upscaler::Reset()
	{
		*this = Fsr1Upscaler{};
	}

	bool Fsr1Upscaler::Prepare(nvrhi::IDevice* device, glm::uvec2 targetSize, nvrhi::IFramebuffer* output,
							   nvrhi::IShader* fullscreenVS, nvrhi::IShader* tonemapPS, nvrhi::IBindingLayout* tonemapLayout)
	{
		if (m_Failed)
			return false;
		if (m_RcasSet)
			return true;
		auto fail = [&]() {
			Reset();
			m_Failed = true;
			BS_CORE_ERROR("SceneRenderer: failed to create FSR 1 resources");
			return false;
		};
		auto makeTarget = [&](uint32_t width, uint32_t height, const char* name) {
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = name;
			return device->createTexture(desc);
		};
		const auto& outputInfo = output->getFramebufferInfo();
		const nvrhi::TextureHandle input = makeTarget(targetSize.x, targetSize.y, "FSR1Input");
		const nvrhi::TextureHandle easu = makeTarget(outputInfo.width, outputInfo.height, "FSR1EASU");
		if (!input || !easu)
			return fail();
		m_InputFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(input));
		m_EasuFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(easu));
		if (!m_InputFramebuffer || !m_EasuFramebuffer)
			return fail();

		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::Pixel;
		layoutDesc.bindingOffsets = ZeroBindingOffsets;
		layoutDesc.registerSpaceIsDescriptorSet = true;
		layoutDesc.bindings = { nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(1),
								nvrhi::BindingLayoutItem::PushConstants(PushConstantSlot, sizeof(FsrPush)) };
		const nvrhi::BindingLayoutHandle layout = device->createBindingLayout(layoutDesc);
		const nvrhi::ShaderHandle easuPS = CreateEmbeddedShader(device, "FSR1EASU.frag", nvrhi::ShaderType::Pixel);
		const nvrhi::ShaderHandle rcasPS = CreateEmbeddedShader(device, "FSR1RCAS.frag", nvrhi::ShaderType::Pixel);
		const nvrhi::SamplerHandle sampler = device->createSampler(nvrhi::SamplerDesc().setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
		if (!layout || !easuPS || !rcasPS || !sampler)
			return fail();
		auto pipeline = [&](nvrhi::IShader* shader, nvrhi::IBindingLayout* bindings, nvrhi::IFramebuffer* framebuffer) {
			nvrhi::GraphicsPipelineDesc desc;
			desc.VS = fullscreenVS;
			desc.PS = shader;
			desc.bindingLayouts = { bindings };
			desc.renderState.rasterState.setCullNone();
			desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
			return device->createGraphicsPipeline(desc, framebuffer->getFramebufferInfo());
		};
		m_TonemapPipeline = pipeline(tonemapPS, tonemapLayout, m_InputFramebuffer);
		m_EasuPipeline = pipeline(easuPS, layout, m_EasuFramebuffer);
		m_RcasPipeline = pipeline(rcasPS, layout, output);
		if (!m_TonemapPipeline || !m_EasuPipeline || !m_RcasPipeline)
			return fail();
		auto bindingSet = [&](nvrhi::ITexture* texture) {
			return device->createBindingSet(nvrhi::BindingSetDesc().setTrackLiveness(true).addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture)).addItem(nvrhi::BindingSetItem::Sampler(1, sampler)).addItem(nvrhi::BindingSetItem::PushConstants(PushConstantSlot, sizeof(FsrPush))), layout);
		};
		m_EasuSet = bindingSet(input);
		m_RcasSet = bindingSet(easu);
		if (!m_EasuSet || !m_RcasSet)
			return fail();
		return true;
	}

	void Fsr1Upscaler::Render(nvrhi::ICommandList* commands, nvrhi::IFramebuffer* output, glm::uvec2 renderSize, float sharpness) const
	{
		const auto& info = output->getFramebufferInfo();
		// Direct C++ callers can bypass scene validation; never send NaN to RCAS.
		const FsrPush push{ glm::vec2(renderSize), { info.width, info.height }, std::isfinite(sharpness) ? std::clamp(sharpness, 0.0f, 2.0f) : 0.2f };
		auto draw = [&](nvrhi::IGraphicsPipeline* pipeline, nvrhi::IFramebuffer* framebuffer, nvrhi::IBindingSet* bindings) {
			nvrhi::GraphicsState state;
			state.pipeline = pipeline;
			state.framebuffer = framebuffer;
			state.bindings = { bindings };
			state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(info.width), static_cast<float>(info.height)));
			commands->setGraphicsState(state);
			commands->setPushConstants(&push, sizeof(push));
			commands->draw(nvrhi::DrawArguments().setVertexCount(3));
		};
		draw(m_EasuPipeline, m_EasuFramebuffer, m_EasuSet);
		draw(m_RcasPipeline, output, m_RcasSet);
	}

} // namespace Basalt
