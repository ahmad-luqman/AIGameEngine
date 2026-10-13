#include "Basalt/Renderer/ShaderUtils.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Renderer/EmbeddedShaders.h"

#include <string>

namespace Basalt {

	nvrhi::ShaderHandle CreateEmbeddedShader(nvrhi::IDevice* device, std::string_view name, nvrhi::ShaderType type)
	{
		const std::span<const uint32_t> code = EmbeddedShaders::Get(name);
		if (code.empty())
		{
			BS_CORE_ERROR("Embedded shader '{}' not found", name);
			return nullptr;
		}

		const std::string debugName(name);
		nvrhi::ShaderDesc desc;
		desc.shaderType = type;
		desc.debugName = debugName;
		desc.entryName = "main";
		return device->createShader(desc, code.data(), code.size_bytes());
	}

	void DrawFullscreen(nvrhi::ICommandList* commandList, nvrhi::IGraphicsPipeline* pipeline, nvrhi::IFramebuffer* framebuffer, nvrhi::IBindingSet* set, const nvrhi::Viewport& viewport, const void* push, size_t pushSize)
	{
		nvrhi::GraphicsState state;
		state.pipeline = pipeline;
		state.framebuffer = framebuffer;
		state.bindings = { set };
		state.viewport.addViewportAndScissorRect(viewport);
		commandList->setGraphicsState(state);
		if (push)
			commandList->setPushConstants(push, pushSize);
		commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
	}

	nvrhi::BindingLayoutDesc MakeLayout(uint32_t set, nvrhi::ShaderType visibility)
	{
		nvrhi::BindingLayoutDesc desc;
		desc.visibility = visibility;
		desc.bindingOffsets = ZeroBindingOffsets;
		desc.registerSpace = set;
		desc.registerSpaceIsDescriptorSet = true;
		return desc;
	}

	nvrhi::GraphicsPipelineHandle CreateFullscreenPipeline(nvrhi::IDevice* device, nvrhi::IShader* vertexShader,
														   nvrhi::IShader* pixelShader, nvrhi::IBindingLayout* layout, nvrhi::IFramebuffer* framebuffer, bool depthTest)
	{
		nvrhi::GraphicsPipelineDesc desc;
		desc.VS = vertexShader;
		desc.PS = pixelShader;
		desc.renderState.rasterState.setCullNone();
		desc.renderState.depthStencilState.setDepthTestEnable(depthTest).setDepthWriteEnable(false).setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
		desc.bindingLayouts = { layout };
		return device->createGraphicsPipeline(desc, framebuffer->getFramebufferInfo());
	}

}
