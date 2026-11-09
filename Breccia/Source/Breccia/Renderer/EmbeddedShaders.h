#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Breccia {

	// SPIR-V compiled from Breccia/Shaders at build time, looked up by source file name ("ImGui.vert").
	class EmbeddedShaders
	{
	public:
		// Empty span if no shader with that name was embedded.
		static std::span<const uint32_t> Get(std::string_view name);
		static std::vector<std::string_view> GetNames();
	};

}
