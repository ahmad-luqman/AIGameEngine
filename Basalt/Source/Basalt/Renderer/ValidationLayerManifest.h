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
			// The copy is in the output folder (written now, or already up to date).
			Written,
			// Nothing to do: no manifest (no Message), one whose library_path already names a folder (no Message), or
			// one naming a library missing from <prefix>/lib (Message says so).
			NotNeeded,
			// The manifest could not be read, is not JSON or has no layer.library_path, or the copy could not be
			// written: Message.
			Failed
		};

		// A default CopyResult means NotNeeded, with nothing to say.
		struct CopyResult
		{
			CopyOutcome Outcome = CopyOutcome::NotNeeded;
			std::string Message;
		};

		// Reads <prefix>/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json. When its library_path is a bare
		// file name and <prefix>/lib/<library_path> exists, writes a copy with the absolute path into outputDirectory
		// (created if needed; an identical copy is left alone, and concurrent writers each replace it atomically).
		[[nodiscard]] CopyResult WriteAbsoluteCopy(const std::filesystem::path& prefix, const std::filesystem::path& outputDirectory);

		// The Homebrew prefixes to probe, each once: $HOMEBREW_PREFIX (when set), /opt/homebrew (Apple silicon) and
		// /usr/local (Intel, harmless: its lib folder is on dyld's default path).
		std::vector<std::filesystem::path> HomebrewPrefixes(const std::optional<std::string>& homebrewPrefix);

		// The folder under `root` that holds the copy for a prefix: one per prefix, so processes using different
		// prefixes (e.g. an x86_64 one under Rosetta) never replace each other's.
		std::filesystem::path CopyDirectory(const std::filesystem::path& root, const std::filesystem::path& prefix);

		// Tries the prefixes in order and returns the folder of the first copy written, or nullopt. outMessages gets one
		// line per prefix that had a manifest but gave no copy ("<prefix>: <why>"), for the log.
		std::optional<std::filesystem::path> FirstAbsoluteCopy(const std::vector<std::filesystem::path>& prefixes, const std::filesystem::path& root, std::vector<std::string>& outMessages);

		// A search path list (entries separated by `separator`) with `directory` appended, so entries a user set keep
		// their precedence; unchanged when the directory is already one of its entries. Empty entries are dropped.
		std::string AppendSearchPath(const std::string& existing, const std::string& directory, char separator);

	}

}
