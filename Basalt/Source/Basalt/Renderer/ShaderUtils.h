#pragma once

#include <nvrhi/nvrhi.h>

#include <string_view>

namespace Basalt {

	// Creates an nvrhi shader from an embedded SPIR-V blob (entry point "main").
	// Returns nullptr and logs if the blob does not exist.
	nvrhi::ShaderHandle CreateEmbeddedShader(nvrhi::IDevice* device, std::string_view name, nvrhi::ShaderType type);

	// Binding layout descriptors use these offsets so GLSL "binding = N" equals the nvrhi slot N for
	// every resource type. See AGENTS.md ("Shader binding convention").
	inline constexpr nvrhi::VulkanBindingOffsets ZeroBindingOffsets = nvrhi::VulkanBindingOffsets()
																		  .setShaderResourceOffset(0)
																		  .setSamplerOffset(0)
																		  .setConstantBufferOffset(0)
																		  .setUnorderedAccessViewOffset(0);

}
