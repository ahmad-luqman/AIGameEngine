#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Basalt {

	// Whole-file read/write helpers. All functions report failure through their return value.
	class FileSystem
	{
	public:
		static std::optional<std::string> ReadTextFile(const std::filesystem::path& path);
		static std::optional<std::vector<uint8_t>> ReadBinaryFile(const std::filesystem::path& path);

		// Writes atomically: data goes to a temporary sibling file (<name>.<process>-<count>.tmp, so concurrent writers
		// never share one) that then replaces the target, so a crash never leaves a half-written scene or project on
		// disk (only, at worst, that temporary file). The target's folder is created when missing.
		// These log the reason of a failure.
		static bool WriteTextFile(const std::filesystem::path& path, std::string_view text);
		static bool WriteBinaryFile(const std::filesystem::path& path, const void* data, size_t size);
		// Returns the reason of a failure instead of logging it.
		static bool WriteTextFile(const std::filesystem::path& path, std::string_view text, std::string& outError);
		static bool WriteBinaryFile(const std::filesystem::path& path, const void* data, size_t size, std::string& outError);

		static bool Exists(const std::filesystem::path& path);
		// Inspects root/relative one component at a time, so a linked ancestor directory counts too. Returns true and
		// sets outEntry at the first symlink, or at the first entry that cannot be inspected (outError set; a
		// missing entry is not an error). False when the whole path is free of links.
		static bool FindSymlink(const std::filesystem::path& root, const std::filesystem::path& relative, std::filesystem::path& outEntry, std::error_code& outError);
		static std::filesystem::path GetExecutableDirectory();
		// The path as UTF-8 text, for messages (path::string() throws on Windows for names outside the ANSI code page).
		static std::string PathToUtf8(const std::filesystem::path& path);
		// The path as '/'-separated UTF-8 text on every platform: the form of asset keys ("Assets/Models/Ship.gltf").
		static std::string PathToGenericUtf8(const std::filesystem::path& path);
		// A path from UTF-8 text (asset references, JSON); std::filesystem::path(std::string) would read the ANSI
		// code page on Windows.
		static std::filesystem::path PathFromUtf8(const std::string& text);
		// The extension in lower case (".png" for "Wall.PNG"), for matching file types.
		static std::string GetLowercaseExtension(const std::filesystem::path& path);
		// A-Z folded to a-z, other bytes (UTF-8 included) unchanged, whatever the C locale: for case-insensitive file
		// name and extension matches.
		static std::string ToLowerAscii(std::string text);
		// Normalized, '/'-separated UTF-8 form of a relative path ("Assets/./A/../B.png" -> "Assets/B.png"): the form of
		// asset keys and FileWatcher paths.
		static std::string NormalizeGeneric(const std::string& path);

		// How a relative path matches the files on disk, compared one component at a time with exact UTF-8 names, so a
		// case-insensitive file system (Windows, macOS) cannot hide a misspelling that breaks a game on Linux.
		enum class PathMatch
		{
			Exact,
			DifferentCase,
			Missing,
			// A folder could not be listed or the path is not a file (outProblem says which).
			Unreadable
		};
		// Directory listings FindExactCase reads, kept across lookups (a validator checks many paths in a folder).
		class DirectoryListings
		{
		public:
			// The entries of a folder (empty for a missing one), or nullptr with the error.
			const std::vector<std::string>* Get(const std::filesystem::path& directory, std::string& outError);

		private:
			struct Listing
			{
				std::vector<std::string> Names;
				std::string Error;
			};
			std::map<std::string, Listing> m_Listings;
		};
		// root/relative against the disk; outActual receives the path as spelled on disk ('/'-separated) for Exact and
		// DifferentCase.
		static PathMatch FindExactCase(const std::filesystem::path& root, const std::filesystem::path& relative, std::string& outActual, std::string& outProblem, DirectoryListings* listings = nullptr);

	private:
		static bool WriteAtomically(const std::filesystem::path& path, const void* data, size_t size, std::string& outError);
	};

}
