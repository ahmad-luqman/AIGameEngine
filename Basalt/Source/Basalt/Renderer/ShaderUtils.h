#pragma once

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <string_view>

namespace Basalt {

	// Zero-offset descriptor-set layout shared by graphics and compute passes.
	nvrhi::BindingLayoutDesc MakeLayout(uint32_t set, nvrhi::ShaderType visibility);
	// Fullscreen triangle pipeline; optional read-only depth test also supports the skybox.
	nvrhi::GraphicsPipelineHandle CreateFullscreenPipeline(nvrhi::IDevice* device, nvrhi::IShader* vertexShader,
														   nvrhi::IShader* pixelShader, nvrhi::IBindingLayout* layout, nvrhi::IFramebuffer* framebuffer, bool depthTest = false);
	void DrawFullscreen(nvrhi::ICommandList* commandList, nvrhi::IGraphicsPipeline* pipeline,
						nvrhi::IFramebuffer* framebuffer, nvrhi::IBindingSet* set, const nvrhi::Viewport& viewport,
						const void* push = nullptr, size_t pushSize = 0);

	// Creates an nvrhi shader from an embedded SPIR-V blob (entry point "main").
	// Returns nullptr and logs if the blob does not exist.
	nvrhi::ShaderHandle CreateEmbeddedShader(nvrhi::IDevice* device, std::string_view name, nvrhi::ShaderType type);

	// Push constants in nvrhi occupy a constant-buffer register; use this slot for them in layouts that
	// also contain ConstantBuffer items.
	inline constexpr uint32_t PushConstantSlot = 31;

	// Binding layout descriptors use these offsets so GLSL "binding = N" equals the nvrhi slot N for
	// every resource type. See AGENTS.md ("Shader binding convention").
	inline constexpr nvrhi::VulkanBindingOffsets ZeroBindingOffsets = nvrhi::VulkanBindingOffsets()
																		  .setShaderResourceOffset(0)
																		  .setSamplerOffset(0)
																		  .setConstantBufferOffset(0)
																		  .setUnorderedAccessViewOffset(0);

}
