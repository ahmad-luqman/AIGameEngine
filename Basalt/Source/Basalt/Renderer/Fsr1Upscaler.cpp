#include "Basalt/Renderer/Fsr1Upscaler.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Renderer/Fsr1Settings.h"
#include "Basalt/Renderer/ShaderUtils.h"

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

	void Fsr1Upscaler::ReleaseResources()
	{
		m_InputFramebuffer = nullptr;
		m_EasuFramebuffer = nullptr;
		m_EasuPipeline = nullptr;
		m_RcasPipeline = nullptr;
		m_EasuSet = nullptr;
		m_RcasSet = nullptr;
	}

	void Fsr1Upscaler::SetActive(bool active)
	{
		if (active && !m_Active && m_Failed)
			Reset();
		m_Active = active;
	}

	bool Fsr1Upscaler::OutputMatches(const nvrhi::FramebufferInfoEx& info) const
	{
		return info == m_OutputInfo && info.width == m_OutputInfo.width && info.height == m_OutputInfo.height && info.arraySize == m_OutputInfo.arraySize;
	}

	bool Fsr1Upscaler::Prepare(nvrhi::IDevice* device, glm::uvec2 targetSize, nvrhi::IFramebuffer* output,
							   nvrhi::IShader* fullscreenVS, nvrhi::ISampler* sampler)
	{
		BS_CORE_ASSERT(device && output && fullscreenVS && sampler, "FSR 1 requires a device, output, shader and sampler");
		const auto& outputInfo = output->getFramebufferInfo();
		if (device != m_Device || targetSize != m_TargetSize || !OutputMatches(outputInfo) || fullscreenVS != m_FullscreenVS || sampler != m_Sampler)
		{
			ReleaseResources();
			m_Failed = false;
			m_Device = device;
			m_TargetSize = targetSize;
			m_OutputInfo = outputInfo;
			m_FullscreenVS = fullscreenVS;
			m_Sampler = sampler;
		}
		if (m_Failed)
			return false;
		if (m_RcasSet)
			return true;
		auto fail = [&](const char* stage) {
			ReleaseResources();
			m_Failed = true;
			// GPU capture harnesses recognize this prefix as a failed render.
			BS_CORE_ERROR("SceneRenderer: failed to create FSR 1 {} (target {}x{}, output {}x{})", stage, targetSize.x, targetSize.y, outputInfo.width, outputInfo.height);
			return false;
		};
		if (targetSize.x == 0 || targetSize.y == 0 || outputInfo.width == 0 || outputInfo.height == 0)
			return fail("targets: invalid dimensions");
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
		const nvrhi::TextureHandle input = makeTarget(targetSize.x, targetSize.y, "FSR1Input");
		if (!input)
			return fail("input texture");
		const nvrhi::TextureHandle easu = makeTarget(outputInfo.width, outputInfo.height, "FSR1EASU");
		if (!easu)
			return fail("EASU texture");
		m_InputFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(input));
		if (!m_InputFramebuffer)
			return fail("input framebuffer");
		m_EasuFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(easu));
		if (!m_EasuFramebuffer)
			return fail("EASU framebuffer");

		nvrhi::BindingLayoutDesc layoutDesc = MakeLayout(0, nvrhi::ShaderType::Pixel);
		layoutDesc.bindings = { nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(1),
								nvrhi::BindingLayoutItem::PushConstants(PushConstantSlot, sizeof(FsrPush)) };
		const nvrhi::BindingLayoutHandle layout = device->createBindingLayout(layoutDesc);
		if (!layout)
			return fail("binding layout");
		const nvrhi::ShaderHandle easuPS = CreateEmbeddedShader(device, "FSR1EASU.frag", nvrhi::ShaderType::Pixel);
		if (!easuPS)
			return fail("EASU shader");
		const nvrhi::ShaderHandle rcasPS = CreateEmbeddedShader(device, "FSR1RCAS.frag", nvrhi::ShaderType::Pixel);
		if (!rcasPS)
			return fail("RCAS shader");
		m_EasuPipeline = CreateFullscreenPipeline(device, fullscreenVS, easuPS, layout, m_EasuFramebuffer);
		if (!m_EasuPipeline)
			return fail("EASU pipeline");
		m_RcasPipeline = CreateFullscreenPipeline(device, fullscreenVS, rcasPS, layout, output);
		if (!m_RcasPipeline)
			return fail("RCAS pipeline");
		auto bindingSet = [&](nvrhi::ITexture* texture) {
			return device->createBindingSet(nvrhi::BindingSetDesc().setTrackLiveness(true).addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture)).addItem(nvrhi::BindingSetItem::Sampler(1, sampler)).addItem(nvrhi::BindingSetItem::PushConstants(PushConstantSlot, sizeof(FsrPush))), layout);
		};
		m_EasuSet = bindingSet(input);
		if (!m_EasuSet)
			return fail("EASU bindings");
		m_RcasSet = bindingSet(easu);
		if (!m_RcasSet)
			return fail("RCAS bindings");
		return true;
	}

	void Fsr1Upscaler::Render(nvrhi::ICommandList* commands, nvrhi::IFramebuffer* output, glm::uvec2 renderSize, float sharpness) const
	{
		BS_CORE_ASSERT(m_RcasSet && output && OutputMatches(output->getFramebufferInfo()), "FSR 1 Render requires a matching successful Prepare");
		BS_CORE_ASSERT(renderSize.x > 0 && renderSize.y > 0 && renderSize.x <= m_TargetSize.x && renderSize.y <= m_TargetSize.y, "FSR 1 render area must fit the prepared target");
		const FsrPush push{ glm::vec2(renderSize), { m_OutputInfo.width, m_OutputInfo.height }, SanitizeFsr1Sharpness(sharpness) };
		const auto viewport = m_EasuFramebuffer->getFramebufferInfo().getViewport();
		DrawFullscreen(commands, m_EasuPipeline, m_EasuFramebuffer, m_EasuSet, viewport, &push, sizeof(push));
		DrawFullscreen(commands, m_RcasPipeline, output, m_RcasSet, viewport, &push, sizeof(push));
	}

}
