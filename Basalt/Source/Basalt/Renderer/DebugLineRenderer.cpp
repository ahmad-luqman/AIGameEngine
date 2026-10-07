#include "Basalt/Renderer/DebugLineRenderer.h"

#include "Basalt/Renderer/ShaderUtils.h"

#include <algorithm>

namespace Basalt {

	bool DebugLineRenderer::Initialize(nvrhi::IDevice* device)
	{
		m_Device = device;
		m_VertexShader = CreateEmbeddedShader(device, "Lines.vert", nvrhi::ShaderType::Vertex);
		m_PixelShader = CreateEmbeddedShader(device, "Lines.frag", nvrhi::ShaderType::Pixel);
		if (!m_VertexShader || !m_PixelShader)
			return false;

		const nvrhi::VertexAttributeDesc attributes[] = {
			nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(offsetof(DebugLineVertex, Position)).setElementStride(sizeof(DebugLineVertex)),
			nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(offsetof(DebugLineVertex, Color)).setElementStride(sizeof(DebugLineVertex)),
		};
		m_InputLayout = device->createInputLayout(attributes, 2, m_VertexShader);

		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::Vertex;
		layoutDesc.bindingOffsets = ZeroBindingOffsets;
		layoutDesc.bindings = { nvrhi::BindingLayoutItem::PushConstants(PushConstantSlot, sizeof(glm::mat4)) };
		m_BindingLayout = device->createBindingLayout(layoutDesc);
		m_BindingSet = device->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::PushConstants(PushConstantSlot, sizeof(glm::mat4))), m_BindingLayout);
		return m_InputLayout && m_BindingLayout && m_BindingSet;
	}

	nvrhi::IGraphicsPipeline* DebugLineRenderer::GetPipeline(nvrhi::IFramebuffer* framebuffer, bool depthTest)
	{
		const int index = depthTest ? 1 : 0;
		const nvrhi::FramebufferInfo& info = framebuffer->getFramebufferInfo();
		if (m_Pipelines[index] && m_PipelineInfo[index] == info)
			return m_Pipelines[index];

		nvrhi::GraphicsPipelineDesc desc;
		desc.primType = nvrhi::PrimitiveType::LineList;
		desc.inputLayout = m_InputLayout;
		desc.VS = m_VertexShader;
		desc.PS = m_PixelShader;
		desc.renderState.rasterState.setCullNone();
		desc.renderState.depthStencilState.setDepthTestEnable(depthTest && info.depthFormat != nvrhi::Format::UNKNOWN).setDepthWriteEnable(false).setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
		desc.renderState.blendState.targets[0].setBlendEnable(true).setSrcBlend(nvrhi::BlendFactor::SrcAlpha).setDestBlend(nvrhi::BlendFactor::InvSrcAlpha).setSrcBlendAlpha(nvrhi::BlendFactor::One).setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
		desc.bindingLayouts = { m_BindingLayout };
		m_Pipelines[index] = m_Device->createGraphicsPipeline(desc, info);
		m_PipelineInfo[index] = info;
		return m_Pipelines[index];
	}

	void DebugLineRenderer::Render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, const glm::mat4& viewProjection, const std::vector<DebugLineVertex>& vertices, bool depthTest)
	{
		if (vertices.size() < 2 || !framebuffer)
			return;

		const size_t bytes = vertices.size() * sizeof(DebugLineVertex);
		if (!m_VertexBuffer || m_VertexBuffer->getDesc().byteSize < bytes)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = std::max<size_t>(bytes * 2, size_t{ 64 } * 1024);
			desc.isVertexBuffer = true;
			desc.debugName = "DebugLines";
			desc.initialState = nvrhi::ResourceStates::VertexBuffer;
			desc.keepInitialState = true;
			m_VertexBuffer = m_Device->createBuffer(desc);
		}
		commandList->writeBuffer(m_VertexBuffer, vertices.data(), bytes);

		nvrhi::IGraphicsPipeline* pipeline = GetPipeline(framebuffer, depthTest);
		if (!pipeline)
			return;
		const nvrhi::FramebufferInfoEx& info = framebuffer->getFramebufferInfo();
		nvrhi::GraphicsState state;
		state.pipeline = pipeline;
		state.framebuffer = framebuffer;
		state.bindings = { m_BindingSet };
		state.vertexBuffers = { nvrhi::VertexBufferBinding().setBuffer(m_VertexBuffer).setSlot(0).setOffset(0) };
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(info.width), static_cast<float>(info.height)));
		commandList->setGraphicsState(state);
		commandList->setPushConstants(&viewProjection, sizeof(glm::mat4));
		commandList->draw(nvrhi::DrawArguments().setVertexCount(static_cast<uint32_t>(vertices.size() & ~size_t(1))));
	}

}
