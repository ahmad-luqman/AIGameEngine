#pragma once

#include "Basalt/Renderer/DebugDraw.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <vector>

namespace Basalt {

	// Draws line lists (DebugDraw, editor grid and gizmos) into a framebuffer that has a depth
	// attachment (SceneRenderer::GetOverlayFramebuffer), depth-tested but not depth-writing.
	class DebugLineRenderer
	{
	public:
		bool Initialize(nvrhi::IDevice* device);
		// Records draws into an open command list.
		void Render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, const glm::mat4& viewProjection, const std::vector<DebugLineVertex>& vertices, bool depthTest = true);

	private:
		nvrhi::IGraphicsPipeline* GetPipeline(nvrhi::IFramebuffer* framebuffer, bool depthTest);

	private:
		nvrhi::IDevice* m_Device = nullptr;
		nvrhi::ShaderHandle m_VertexShader;
		nvrhi::ShaderHandle m_PixelShader;
		nvrhi::InputLayoutHandle m_InputLayout;
		nvrhi::BindingLayoutHandle m_BindingLayout;
		nvrhi::BindingSetHandle m_BindingSet;
		nvrhi::BufferHandle m_VertexBuffer;
		nvrhi::GraphicsPipelineHandle m_Pipelines[2];
		nvrhi::FramebufferInfo m_PipelineInfo[2];
	};

}
