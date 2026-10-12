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
	class SaveData
	{
	public:
		static constexpr const char* FileName = "SaveData.json";
		// A malformed file is moved here (next to FileName) so the player's data can still be recovered.
		static constexpr const char* CorruptFileName = "SaveData.corrupt.json";
		static constexpr int FormatVersion = 1;
		// Real save files are a few kilobytes; this bounds the memory a broken or hostile file can take.
		static constexpr size_t MaxFileSize = size_t{ 16 } * 1024 * 1024;

		explicit SaveData(std::filesystem::path path);

		// <Platform::GetUserDataDirectory()>/<SanitizeDirectoryName(projectName)>/SaveData.json, or nullopt
		// (with outError) when the platform reports no user data directory.
		static std::optional<std::filesystem::path> GetDefaultPath(const std::string& projectName, std::string& outError);
		// A project name usable as one directory name on every platform: characters other than letters,
		// digits, space, '-', '_' and '.' become '_', leading/trailing spaces and dots are dropped, Windows
		// device names (CON, NUL, COM1, ...) get a '_' suffix, and an empty result becomes "Untitled".
		static std::string SanitizeDirectoryName(const std::string& name);

		// Replaces the values with the file's. A missing file is empty save data, not an error. A malformed
		// file leaves the values empty, is moved to CorruptFileName, and outError explains both.
		bool Load(std::string& outError);
		// Writes atomically (a crash never leaves a half-written save), creating the directory if needed.
		bool Save(std::string& outError) const;
		// The file format without the file: used by Load/Save and the fuzzer.
		bool LoadFromString(std::string_view text, std::string& outError);
		std::string SaveToString() const;

		// The value stored under key, or null when there is none.
		const nlohmann::json* Find(const std::string& key) const;
		void Set(const std::string& key, nlohmann::json value);
		// Returns whether the key existed.
		bool Remove(const std::string& key);
		void Clear();
		// Every key, sorted.
		std::vector<std::string> GetKeys() const;

		const std::filesystem::path& GetPath() const { return m_Path; }

	private:
		std::filesystem::path m_Path;
		// Always a JSON object.
		nlohmann::json m_Values = nlohmann::json::object();
	};

}
