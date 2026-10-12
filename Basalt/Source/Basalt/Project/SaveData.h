#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Basalt {

	// A game's persistent key-value save data (Lua Storage): one JSON file in the player's per-game data
	// directory (GetDefaultPath). It lives outside the project, so exported games never write into their
	// install directory and scripts never choose a path. Values are JSON; keys are non-empty strings.
	//
	// File layout: { "Format": "BasaltSaveData", "Version": 1, "Values": { "<key>": <value>, ... } }
	//
	// The player's file is never destroyed: one that cannot be read, or that a newer version wrote, blocks Save
	// until a later Load reads a file, finds none, or moves a malformed one aside (to a backup next to it).
	class SaveData
	{
	public:
		static constexpr const char* FileName = "SaveData.json";
		static constexpr const char* FormatName = "BasaltSaveData";
		// Backups of malformed files are "SaveData.corrupt.json", then "SaveData.corrupt.2.json", ... (a new name
		// each time, so every damaged file stays recoverable).
		static constexpr const char* CorruptFileStem = "SaveData.corrupt";
		static constexpr int FormatVersion = 1;
		// Real save files are a few kilobytes; this bounds the memory a broken or hostile file can take. Save
		// refuses to write more than Load accepts.
		static constexpr size_t MaxFileSize = size_t{ 16 } * 1024 * 1024;

		explicit SaveData(std::filesystem::path path);

		// <Platform::GetUserDataDirectory()>/<directory>/SaveData.json, where directory is
		// SanitizeDirectoryName(projectName), plus "-<hash of the name>" when sanitizing changed it (so distinct
		// names never share save data); nullopt (with outError) when there is no usable user data directory.
		static std::optional<std::filesystem::path> GetDefaultPath(const std::string& projectName, std::string& outError);
		// A project name usable as one directory name on every platform: characters other than ASCII letters,
		// digits, space, '-', '_' and '.' become '_', leading/trailing spaces and dots are dropped, a Windows
		// device name (CON, NUL, COM0-9, LPT0-9, ...) gets a '_' after it, before any extension ("NUL.txt" ->
		// "NUL_.txt"), and an empty result becomes "Untitled".
		static std::string SanitizeDirectoryName(const std::string& name);

		// Replaces the values with the file's; a missing file is empty save data. Returns false with outError when
		// - the file is malformed (bad JSON or layout, larger than MaxFileSize): it is moved to a backup and the
		//   values are cleared, so the game starts afresh (if it cannot be moved, the values are kept and Save is
		//   blocked);
		// - it cannot be read, or a newer version wrote it: it is left alone, the values are kept and Save is
		//   blocked.
		bool Load(std::string& outError);
		// Writes atomically (a crash never leaves a half-written save), creating the directory if needed. Fails
		// while blocked by Load, and when the data would exceed MaxFileSize.
		bool Save(std::string& outError) const;
		// The file format without the file (the fuzzer and tests). On failure the values are kept. It does not
		// change whether Save is blocked.
		bool LoadFromString(std::string_view text, std::string& outError);
		std::string SaveToString() const;

		// The value stored under key, or null when there is none. The pointer is valid until the next change.
		const nlohmann::json* Find(const std::string& key) const;
		// Stores a value (null removes the key). False with outError, storing nothing, for what a save could not
		// write back exactly: an empty key, NaN or infinite numbers, strings or keys that are not valid UTF-8.
		[[nodiscard]] bool Set(const std::string& key, nlohmann::json value, std::string& outError);
		// Returns whether the key existed.
		bool Remove(const std::string& key);
		void Clear();
		// Every key, sorted.
		std::vector<std::string> GetKeys() const;

		const std::filesystem::path& GetPath() const { return m_Path; }

	private:
		enum class ParseStatus
		{
			Valid,
			Malformed,
			// A file of a newer FormatVersion (whatever else it contains): not ours to change.
			Newer
		};
		static constexpr size_t MaxFileSizeMB = MaxFileSize / (size_t{ 1024 } * 1024);

		static ParseStatus Parse(std::string_view text, nlohmann::json& outValues, std::string& outError);
		// Moves a malformed file to a fresh backup and starts afresh (or blocks Save when it cannot be moved).
		// Always false: the load failed either way.
		bool MoveMalformedAside(const std::string& where, const std::string& problem, std::string& outError);
		// Records that saving would destroy the player's data. Always false (Load's result).
		bool BlockSave(std::string message, std::string& outError);

	private:
		std::filesystem::path m_Path;
		// Always a JSON object.
		nlohmann::json m_Values = nlohmann::json::object();
		// Why Save would destroy the player's data, while it would.
		std::optional<std::string> m_SaveBlocked;
	};

}
