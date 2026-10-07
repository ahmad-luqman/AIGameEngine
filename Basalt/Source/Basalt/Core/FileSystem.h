#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	// Whole-file read/write helpers. All functions report failure through their return value.
	class FileSystem
	{
	public:
		static std::optional<std::string> ReadTextFile(const std::filesystem::path& path);
		static std::optional<std::vector<uint8_t>> ReadBinaryFile(const std::filesystem::path& path);

		// Writes atomically: data goes to a temporary sibling file that then replaces the target,
		// so a crash never leaves a half-written scene or project on disk.
		static bool WriteTextFile(const std::filesystem::path& path, std::string_view text);
		static bool WriteBinaryFile(const std::filesystem::path& path, const void* data, size_t size);

		static bool Exists(const std::filesystem::path& path);
		static std::filesystem::path GetExecutableDirectory();
	};

}
