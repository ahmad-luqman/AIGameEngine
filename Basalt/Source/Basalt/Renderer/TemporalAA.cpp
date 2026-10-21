#include "Basalt/Renderer/TemporalAA.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Renderer/ShaderUtils.h"

#include <cstddef>

namespace Basalt {
	namespace {
		struct TemporalPush
		{
			glm::mat4 SkyReprojection;
			glm::uvec2 Size;
			uint32_t Valid;
			uint32_t Padding = 0;
			glm::vec2 JitterDelta;
		};
		static_assert(sizeof(TemporalPush) == 88);
		static_assert(offsetof(TemporalPush, JitterDelta) == 80);
	}

	bool TemporalAA::Prepare(nvrhi::IDevice* device, nvrhi::ITexture* color, nvrhi::ITexture* depth,
							 nvrhi::ITexture* motion, nvrhi::IShader* fullscreen, nvrhi::ISampler* linear, nvrhi::ISampler* point)
	{
		if (m_Failed)
			return false;
		if (m_Source == color && m_SourceDepth == depth && m_Motion == motion && m_Pipeline)
			return true;
		const auto& desc = color->getDesc();
		auto fail = [&](const char* stage) {
			Reset();
			m_Failed = true;
			BS_CORE_ERROR("SceneRenderer: failed to create TAA {} at {}x{}; toggle TAA or resize to retry", stage, desc.width, desc.height);
			return false;
		};
		m_Source = color;
		m_SourceDepth = depth;
		m_Motion = motion;
		m_Index = 0;
		auto target = [&](nvrhi::Format format, const char* name) {
			nvrhi::TextureDesc texture;
			texture.width = desc.width;
			texture.height = desc.height;
			texture.format = format;
			texture.isRenderTarget = true;
			texture.initialState = nvrhi::ResourceStates::ShaderResource;
			texture.keepInitialState = true;
			texture.debugName = name;
			return device->createTexture(texture);
		};
		auto layoutDesc = MakeLayout(0, nvrhi::ShaderType::Pixel);
		layoutDesc.bindings = { nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1),
								nvrhi::BindingLayoutItem::Texture_SRV(2), nvrhi::BindingLayoutItem::Texture_SRV(3), nvrhi::BindingLayoutItem::Texture_SRV(4),
								nvrhi::BindingLayoutItem::Sampler(5), nvrhi::BindingLayoutItem::Sampler(6),
								nvrhi::BindingLayoutItem::PushConstants(PushConstantSlot, sizeof(TemporalPush)) };
		const auto layout = device->createBindingLayout(layoutDesc);
		const auto shader = CreateEmbeddedShader(device, "TemporalAA.frag", nvrhi::ShaderType::Pixel);
		if (!layout || !shader)
			return fail(!layout ? "binding layout" : "resolve shader");
		for (uint32_t i = 0; i < 2; i++)
		{
			m_Color[i] = target(nvrhi::Format::RGBA16_FLOAT, "TAAColor");
			m_Depth[i] = target(nvrhi::Format::R32_FLOAT, "TAADepth");
			if (!m_Color[i] || !m_Depth[i])
				return fail(!m_Color[i] ? "color history" : "depth history");
			m_Framebuffers[i] = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_Color[i]).addColorAttachment(m_Depth[i]));
			if (!m_Framebuffers[i])
				return fail("history framebuffer");
		}
		for (uint32_t i = 0; i < 2; i++)
		{
			m_Sets[i] = device->createBindingSet(nvrhi::BindingSetDesc()
													 .addItem(nvrhi::BindingSetItem::Texture_SRV(0, color))
													 .addItem(nvrhi::BindingSetItem::Texture_SRV(1, depth))
													 .addItem(nvrhi::BindingSetItem::Texture_SRV(2, motion))
													 .addItem(nvrhi::BindingSetItem::Texture_SRV(3, m_Color[1 - i]))
													 .addItem(nvrhi::BindingSetItem::Texture_SRV(4, m_Depth[1 - i]))
													 .addItem(nvrhi::BindingSetItem::Sampler(5, linear))
													 .addItem(nvrhi::BindingSetItem::Sampler(6, point))
													 .addItem(nvrhi::BindingSetItem::PushConstants(PushConstantSlot, sizeof(TemporalPush))),
												 layout);
			if (!m_Sets[i])
				return fail("resolve bindings");
		}
		m_Pipeline = CreateFullscreenPipeline(device, fullscreen, shader, layout, m_Framebuffers[0]);
		return m_Pipeline ? true : fail("resolve pipeline");
	}

	void TemporalAA::Render(nvrhi::ICommandList* commands, glm::uvec2 size, bool valid, const glm::mat4& skyReprojection, glm::vec2 jitterDelta)
	{
		BS_CORE_ASSERT(m_Pipeline, "Prepare must succeed before the temporal resolve");
		// Initialize both histories before even an invalid-history binding can observe undefined image contents.
		if (!valid)
		{
			for (uint32_t i = 0; i < 2; i++)
			{
				commands->clearTextureFloat(m_Color[i], nvrhi::AllSubresources, nvrhi::Color(0));
				commands->clearTextureFloat(m_Depth[i], nvrhi::AllSubresources, nvrhi::Color(1));
			}
		}
		const TemporalPush push{ skyReprojection, size, valid ? 1u : 0u, 0, jitterDelta };
		DrawFullscreen(commands, m_Pipeline, m_Framebuffers[m_Index], m_Sets[m_Index],
					   nvrhi::Viewport(static_cast<float>(size.x), static_cast<float>(size.y)), &push, sizeof(push));
		commands->copyTexture(m_Source, nvrhi::TextureSlice(), m_Color[m_Index], nvrhi::TextureSlice());
		m_Index = 1 - m_Index;
	}
}
