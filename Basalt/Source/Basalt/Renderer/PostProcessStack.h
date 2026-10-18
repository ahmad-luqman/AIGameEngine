#pragma once

#include "Basalt/Core/MemoryTracker.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <array>
#include <map>
#include <vector>

namespace Basalt {
	struct PostProcessComponent;

	// Per-renderer HDR bloom/exposure resources and perceptual-color FXAA. No CPU readback or shared history.
	class PostProcessStack
	{
	public:
		bool Prepare(nvrhi::IDevice* device, nvrhi::ITexture* hdr, nvrhi::IShader* fullscreen, nvrhi::ISampler* sampler);
		void RenderHDR(nvrhi::ICommandList* commands, glm::uvec2 size, const PostProcessComponent& settings, float deltaTime, bool resetExposure);
		nvrhi::ITexture* GetHDR() const { return m_Composite; }
		bool PrepareFXAA(nvrhi::IFramebuffer* output);
		nvrhi::IFramebuffer* GetFXAAInput() const { return m_FxaaInput; }
		void RenderFXAA(nvrhi::ICommandList* commands, nvrhi::IFramebuffer* output, glm::uvec2 size, bool finalOutput);
		void Reset() { *this = PostProcessStack{}; }
		// Failed allocations stay failed until all effects are disabled and re-enabled, or targets change.
		void Retry()
		{
			if (m_Failed)
				Reset();
		}

	private:
		static constexpr size_t MaxBloomLevels = 6;
		bool Fail(const char* stage);
		nvrhi::IDevice* m_Device = nullptr;
		nvrhi::TextureHandle m_Source, m_Composite, m_Exposure;
		nvrhi::ShaderHandle m_Fullscreen;
		nvrhi::SamplerHandle m_Sampler;
		nvrhi::FramebufferHandle m_CompositeFramebuffer, m_FxaaInput;
		nvrhi::BindingLayoutHandle m_BloomLayout, m_CompositeLayout, m_ExposureLayout, m_FxaaLayout;
		nvrhi::GraphicsPipelineHandle m_BloomPipeline, m_CompositePipeline, m_FxaaPipeline;
		// Retain both native-output and FSR-input pipelines across dynamic-resolution crossings.
		std::map<nvrhi::Format, nvrhi::GraphicsPipelineHandle> m_FxaaPipelines;
		nvrhi::BindingSetHandle m_CompositeSet, m_ExposureSet, m_FxaaSet;
		nvrhi::BufferHandle m_Histogram;
		nvrhi::ComputePipelineHandle m_HistogramPipeline, m_ReducePipeline;
		struct BloomLevel
		{
			nvrhi::TextureHandle Down, Up;
			nvrhi::FramebufferHandle DownFramebuffer, UpFramebuffer;
			nvrhi::BindingSetHandle DownSet, UpSet;
		};
		std::vector<BloomLevel> m_Bloom;
		bool m_AutoLastFrame = false;
		bool m_Failed = false;
		MemoryAllocation m_HistogramMemory{ MemoryCategory::GpuBuffers };
	};
}
