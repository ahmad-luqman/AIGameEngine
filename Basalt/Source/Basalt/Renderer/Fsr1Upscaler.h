#pragma once

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

namespace Basalt {

	// Owns perceptual-color intermediate targets and EASU/RCAS passes. The caller tonemaps into the
	// input framebuffer. Prepare caches target/output dimensions, framebuffer formats, device, vertex
	// shader and sampler, and rebuilds on any mismatch (framebuffer identity is irrelevant).
	// A failed preparation logs its stage once and stays failed for that configuration until Reset or
	// an inactive -> active transition. Inactivity alone retains successful resources.
	class Fsr1Upscaler
	{
	public:
		bool Prepare(nvrhi::IDevice* device, glm::uvec2 targetSize, nvrhi::IFramebuffer* output,
					 nvrhi::IShader* fullscreenVS, nvrhi::ISampler* sampler);
		void SetActive(bool active);
		void Reset();
		nvrhi::IFramebuffer* GetInputFramebuffer() const { return m_InputFramebuffer; }
		// Requires successful Prepare, compatible output info and 1 <= renderSize <= prepared targetSize.
		void Render(nvrhi::ICommandList* commands, nvrhi::IFramebuffer* output, glm::uvec2 renderSize, float sharpness) const;

	private:
		bool OutputMatches(const nvrhi::FramebufferInfoEx& info) const;
		void ReleaseResources();

		nvrhi::FramebufferHandle m_InputFramebuffer, m_EasuFramebuffer;
		nvrhi::GraphicsPipelineHandle m_EasuPipeline, m_RcasPipeline;
		nvrhi::BindingSetHandle m_EasuSet, m_RcasSet;
		nvrhi::IDevice* m_Device = nullptr;
		nvrhi::ShaderHandle m_FullscreenVS;
		nvrhi::SamplerHandle m_Sampler;
		glm::uvec2 m_TargetSize{};
		nvrhi::FramebufferInfoEx m_OutputInfo;
		bool m_Failed = false;
		bool m_Active = false;
	};

}
