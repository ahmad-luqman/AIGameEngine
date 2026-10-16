#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace Basalt {

	// Homebrew installs the Khronos validation layer with a manifest whose library_path is a bare file name, so the
	// Vulkan loader can only load it when the dynamic linker searches <prefix>/lib, i.e. with
	// DYLD_FALLBACK_LIBRARY_PATH set at launch (a running process cannot change its own dyld search path). Instead,
	// the device points VK_ADD_LAYER_PATH at a copy of the manifest that names the library by absolute path.
	namespace ValidationLayerManifest {

		inline constexpr const char* FileName = "VkLayer_khronos_validation.json";

		// Reads <prefix>/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json. When its library_path is
		// relative and <prefix>/lib/<library_path> exists, writes a copy with the absolute path into
		// outputDirectory (created if needed; an identical copy is left alone, so concurrent processes share it)
		// and returns outputDirectory, the folder to add to the layer search. nullopt with an empty outError when no
		// copy is needed (no such manifest, an absolute path already, no library to point at); nullopt with
		// outError when the manifest cannot be read or the copy written.
		std::optional<std::filesystem::path> WriteAbsoluteCopy(const std::filesystem::path& prefix, const std::filesystem::path& outputDirectory, std::string& outError);

	}

}
