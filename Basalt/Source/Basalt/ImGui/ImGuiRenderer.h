#pragma once

#include <nvrhi/nvrhi.h>

#include <unordered_map>

struct ImDrawData;
struct ImTextureData;

namespace Basalt {

	// Dear ImGui renderer backend on top of nvrhi. Supports ImGuiBackendFlags_RendererHasTextures
	// (dynamic font atlas) and ImGuiBackendFlags_RendererHasVtxOffset.
	//
	// ImTextureID values are nvrhi::ITexture pointers. User textures shown through ImGui::Image must be
	// shader-resource textures that stay alive until the frame that references them has been rendered.
	class ImGuiRenderer
	{
	public:
		bool Init(nvrhi::IDevice* device);
		void Shutdown();

		void Render(ImDrawData* drawData, nvrhi::IFramebuffer* framebuffer);

	private:
		void UpdateTexture(ImTextureData* texture, nvrhi::ICommandList* commandList);
		bool EnsureBuffers(const ImDrawData* drawData);
		bool EnsurePipeline(nvrhi::IFramebuffer* framebuffer);
		nvrhi::IBindingSet* GetBindingSet(nvrhi::ITexture* texture);

	private:
		nvrhi::IDevice* m_Device = nullptr;
		nvrhi::CommandListHandle m_CommandList;

		nvrhi::ShaderHandle m_VertexShader;
		nvrhi::ShaderHandle m_PixelShader;
		nvrhi::InputLayoutHandle m_InputLayout;
		nvrhi::BindingLayoutHandle m_BindingLayout;
		nvrhi::SamplerHandle m_Sampler;
		nvrhi::GraphicsPipelineHandle m_Pipeline;
		nvrhi::FramebufferInfo m_PipelineFramebufferInfo;

		nvrhi::BufferHandle m_VertexBuffer;
		nvrhi::BufferHandle m_IndexBuffer;

		struct CachedBindingSet
		{
			nvrhi::BindingSetHandle BindingSet;
			uint64_t LastUsedFrame = 0;
		};
		std::unordered_map<nvrhi::ITexture*, CachedBindingSet> m_BindingSets;
		uint64_t m_FrameIndex = 0;
	};

}
