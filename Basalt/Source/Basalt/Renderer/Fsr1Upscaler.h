#pragma once

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

namespace Basalt {

	// Owns FSR 1's perceptual-color intermediate targets and EASU/RCAS passes. Resources are allocated
	// only when needed and retained across dynamic render-area changes; Reset releases them on resize.
	class Fsr1Upscaler
	{
	public:
		bool Prepare(nvrhi::IDevice* device, glm::uvec2 targetSize, nvrhi::IFramebuffer* output,
					 nvrhi::IShader* fullscreenVS, nvrhi::IShader* tonemapPS, nvrhi::IBindingLayout* tonemapLayout);
		void Reset();
		nvrhi::IFramebuffer* GetInputFramebuffer() const { return m_InputFramebuffer; }
		nvrhi::IGraphicsPipeline* GetTonemapPipeline() const { return m_TonemapPipeline; }
		void Render(nvrhi::ICommandList* commands, nvrhi::IFramebuffer* output, glm::uvec2 renderSize, float sharpness) const;

	private:
		nvrhi::FramebufferHandle m_InputFramebuffer, m_EasuFramebuffer;
		nvrhi::GraphicsPipelineHandle m_TonemapPipeline, m_EasuPipeline, m_RcasPipeline;
		nvrhi::BindingSetHandle m_EasuSet, m_RcasSet;
		bool m_Failed = false;
	};

} // namespace Basalt
