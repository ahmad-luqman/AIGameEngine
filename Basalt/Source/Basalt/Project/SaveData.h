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
	// The player's file is never destroyed: one that cannot be read, or that a newer version wrote, blocks
	// Save until a later Load succeeds; a malformed one is first moved to a backup next to it.
	class SaveData
	{
	public:
		static constexpr const char* FileName = "SaveData.json";
		// Backups of malformed files are "SaveData.corrupt.json", then "SaveData.corrupt.2.json", ... (never
		// overwritten, so every damaged file stays recoverable).
		static constexpr const char* CorruptFileStem = "SaveData.corrupt";
		static constexpr int FormatVersion = 1;
		// Real save files are a few kilobytes; this bounds the memory a broken or hostile file can take. Save
		// refuses to write more than Load accepts.
		static constexpr size_t MaxFileSize = size_t{ 16 } * 1024 * 1024;

		explicit SaveData(std::filesystem::path path);

		// <Platform::GetUserDataDirectory()>/<SanitizeDirectoryName(projectName)>/SaveData.json, or nullopt
		// (with outError) when there is no usable user data directory.
		static std::optional<std::filesystem::path> GetDefaultPath(const std::string& projectName, std::string& outError);
		// A project name usable as one directory name on every platform: characters other than ASCII letters,
		// digits, space, '-', '_' and '.' become '_', leading/trailing spaces and dots are dropped, a Windows
		// device name (CON, NUL, COM0-9, LPT0-9, ...) gets a '_' after it, before any extension ("NUL.txt" ->
		// "NUL_.txt"), and an empty result becomes "Untitled".
		static std::string SanitizeDirectoryName(const std::string& name);

		// Replaces the values with the file's; a missing file is empty save data. On failure (false, outError)
		// the values are kept, and:
		// - a malformed file (bad JSON or layout, larger than MaxFileSize) is moved to a backup and the values
		//   are cleared, so the game starts afresh (if the move fails, Save stays blocked instead);
		// - a file that cannot be read, or that a newer version wrote, is left alone and Save is blocked.
		bool Load(std::string& outError);
		// Writes atomically (a crash never leaves a half-written save), creating the directory if needed. Fails
		// while blocked by Load, and when the data would exceed MaxFileSize.
		bool Save(std::string& outError) const;
		// The file format without the file (used by Load/Save and the fuzzer). On failure the values are kept.
		bool LoadFromString(std::string_view text, std::string& outError);
		std::string SaveToString() const;

		// The value stored under key, or null when there is none. The pointer is valid until the next change.
		const nlohmann::json* Find(const std::string& key) const;
		// False (nothing stored) for an empty key.
		bool Set(const std::string& key, nlohmann::json value);
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
			// A valid file of a newer FormatVersion: not ours to change.
			Newer
		};
		static ParseStatus Parse(std::string_view text, nlohmann::json& outValues, std::string& outError);

	private:
		std::filesystem::path m_Path;
		// Always a JSON object.
		nlohmann::json m_Values = nlohmann::json::object();
		// Why Save would destroy the player's data, while it would; empty when saving is safe.
		std::string m_SaveBlocked;
	};

}
