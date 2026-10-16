#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	// Homebrew installs the Khronos validation layer with a manifest whose library_path is a bare file name, so the
	// Vulkan loader can only load it when the dynamic linker searches <prefix>/lib. Apple silicon's /opt/homebrew/lib
	// is not on dyld's default fallback path and a running process cannot change its own dyld search path, so instead
	// the device points VK_ADD_LAYER_PATH at a copy of the manifest that names the library by absolute path.
	namespace ValidationLayerManifest {

		inline constexpr const char* FileName = "VkLayer_khronos_validation.json";

		enum class CopyOutcome
		{
			// The copy is in Directory (written now, or already up to date).
			Written,
			// Nothing to do: no manifest under the prefix, or one that works as installed. Message says why when a
			// manifest exists (e.g. it names a library that is not there).
			NotNeeded,
			// The manifest could not be read or parsed, or the copy written: Message.
			Failed
		};

		struct CopyResult
		{
			CopyOutcome Outcome = CopyOutcome::NotNeeded;
			std::filesystem::path Directory;
			std::string Message;
		};

		// Reads <prefix>/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json. When its library_path is a bare
		// file name and <prefix>/lib/<library_path> exists, writes a copy with the absolute path into outputDirectory
		// (created if needed; an identical copy is left alone, and concurrent writers each replace it atomically).
		CopyResult WriteAbsoluteCopy(const std::filesystem::path& prefix, const std::filesystem::path& outputDirectory);

		// The Homebrew prefixes to probe, without duplicates: $HOMEBREW_PREFIX (when set), /opt/homebrew (Apple silicon)
		// and /usr/local (Intel, harmless: its lib folder is on dyld's default path).
		std::vector<std::filesystem::path> HomebrewPrefixes(const std::optional<std::string>& homebrewPrefix);

		// A search path list (entries separated by `separator`) with `directory` appended, so entries a user set keep
		// their precedence; unchanged when the directory is already one of its entries.
		std::string AppendSearchPath(const std::string& existing, const std::string& directory, char separator);

	}

}
