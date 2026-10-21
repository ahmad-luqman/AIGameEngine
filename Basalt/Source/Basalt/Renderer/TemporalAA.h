#pragma once

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <array>

namespace Basalt {
	// Per-renderer HDR color/depth ping-pong resolve, copied back into the source HDR texture.
	// Invalid history clears both targets; Prepare failures remain latched until Reset.
	class TemporalAA
	{
	public:
		bool Prepare(nvrhi::IDevice* device, nvrhi::ITexture* color, nvrhi::ITexture* depth,
					 nvrhi::ITexture* motion, nvrhi::IShader* fullscreen, nvrhi::ISampler* linear, nvrhi::ISampler* point);
		void Render(nvrhi::ICommandList* commands, glm::uvec2 size, bool valid,
					const glm::mat4& skyReprojection, glm::vec2 jitterDelta = glm::vec2(0));
		void Reset() { *this = TemporalAA{}; }

	private:
		bool m_Failed = false;
		uint32_t m_Index = 0;
		nvrhi::TextureHandle m_Source, m_SourceDepth, m_Motion;
		std::array<nvrhi::TextureHandle, 2> m_Color, m_Depth;
		std::array<nvrhi::FramebufferHandle, 2> m_Framebuffers;
		std::array<nvrhi::BindingSetHandle, 2> m_Sets;
		nvrhi::GraphicsPipelineHandle m_Pipeline;
	};
}
