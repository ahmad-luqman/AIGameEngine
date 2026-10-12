#include "Basalt/Project/SaveData.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>
#include <utility>

namespace Basalt {

	SaveData::SaveData(std::filesystem::path path)
		: m_Path(std::move(path))
	{
	}

	std::optional<std::filesystem::path> SaveData::GetDefaultPath(const std::string& projectName, std::string& outError)
	{
		const std::optional<std::filesystem::path> base = Platform::GetUserDataDirectory();
		if (!base)
		{
			outError = std::string("no user data directory: set ") + Platform::UserDataDirectoryVariable + " (or the platform's HOME/APPDATA) to an absolute path";
			return std::nullopt;
		}
		return *base / SanitizeDirectoryName(projectName) / FileName;
	}

	std::string SaveData::SanitizeDirectoryName(const std::string& name)
	{
		std::string result;
		result.reserve(name.size());
		for (const char c : name)
		{
			const auto byte = static_cast<unsigned char>(c);
			const bool allowed = std::isalnum(byte) != 0 || c == ' ' || c == '-' || c == '_' || c == '.';
			// Bytes >= 0x80 (UTF-8) are replaced too: their meaning in a path differs between platforms.
			result += (allowed && byte < 0x80) ? c : '_';
		}

		// Windows strips trailing dots and spaces from names, and a leading dot hides the directory elsewhere.
		const size_t first = result.find_first_not_of(" .");
		if (first == std::string::npos)
			return "Untitled";
		const size_t last = result.find_last_not_of(" .");
		result = result.substr(first, last - first + 1);

		// Device names are reserved on Windows with any extension ("NUL.txt" too).
		static constexpr std::array<std::string_view, 22> Reserved = { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
		std::string stem = result.substr(0, result.find('.'));
		std::transform(stem.begin(), stem.end(), stem.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
		if (std::find(Reserved.begin(), Reserved.end(), stem) != Reserved.end())
			result += '_';
		return result;
	}

	bool SaveData::Load(std::string& outError)
	{
		m_Values = nlohmann::json::object();

		std::error_code error;
		const bool exists = std::filesystem::exists(m_Path, error);
		if (error)
		{
			outError = "cannot access save data '" + m_Path.string() + "': " + error.message();
			return false;
		}
		if (!exists)
			return true;

		const uintmax_t size = std::filesystem::file_size(m_Path, error);
		std::string problem;
		if (error)
		{
			outError = "cannot read save data '" + m_Path.string() + "': " + error.message();
			return false;
		}
		if (size > MaxFileSize)
		{
			problem = "it is larger than " + std::to_string(MaxFileSize / (size_t{ 1024 } * 1024)) + " MB";
		}
		else
		{
			const std::optional<std::string> text = FileSystem::ReadTextFile(m_Path);
			if (!text)
			{
				outError = "cannot read save data '" + m_Path.string() + "'";
				return false;
			}
			if (LoadFromString(*text, problem))
				return true;
		}

		// Keep the unreadable file for recovery instead of overwriting it with the next Save.
		const std::filesystem::path backup = m_Path.parent_path() / CorruptFileName;
		std::filesystem::rename(m_Path, backup, error);
		outError = "save data '" + m_Path.string() + "' is unreadable (" + problem + "); starting with empty save data";
		if (error)
			outError += " (moving it to '" + backup.string() + "' failed: " + error.message() + ")";
		else
			outError += "; the old file was moved to '" + backup.string() + "'";
		return false;
	}

	bool SaveData::Save(std::string& outError) const
	{
		if (!FileSystem::WriteTextFile(m_Path, SaveToString()))
		{
			outError = "cannot write save data '" + m_Path.string() + "'";
			return false;
		}
		return true;
	}

	bool SaveData::LoadFromString(std::string_view text, std::string& outError)
	{
		m_Values = nlohmann::json::object();
		const nlohmann::json data = ParseJson(text);
		if (data.is_discarded())
		{
			outError = "not valid JSON";
			return false;
		}
		if (!data.is_object())
		{
			outError = "the file must contain a JSON object";
			return false;
		}
		for (const auto& [key, value] : data.items())
		{
			if (key != "Format" && key != "Version" && key != "Values")
			{
				outError = "unknown key '" + key + "' (valid: Format, Version, Values)";
				return false;
			}
		}

		const auto format = data.find("Format");
		if (format == data.end() || !format->is_string() || *format != "BasaltSaveData")
		{
			outError = "'Format' must be \"BasaltSaveData\"";
			return false;
		}
		const auto version = data.find("Version");
		if (version == data.end() || !version->is_number_integer() || version->get<int64_t>() < 1)
		{
			outError = "'Version' must be a positive integer";
			return false;
		}
		if (version->get<int64_t>() > FormatVersion)
		{
			outError = "save data version " + std::to_string(version->get<int64_t>()) + " is newer than supported version " + std::to_string(FormatVersion);
			return false;
		}
		const auto values = data.find("Values");
		if (values == data.end() || !values->is_object())
		{
			outError = "'Values' must be an object";
			return false;
		}
		for (const auto& [key, value] : values->items())
		{
			if (key.empty())
			{
				outError = "'Values' has an empty key";
				return false;
			}
		}
		m_Values = *values;
		return true;
	}

	std::string SaveData::SaveToString() const
	{
		const nlohmann::json data = { { "Format", "BasaltSaveData" }, { "Version", FormatVersion }, { "Values", m_Values } };
		// Values may hold strings that are not valid UTF-8 (Lua strings are bytes); replace those bytes rather
		// than failing the whole save.
		return data.dump(1, '\t', false, nlohmann::json::error_handler_t::replace);
	}

	const nlohmann::json* SaveData::Find(const std::string& key) const
	{
		const auto it = m_Values.find(key);
		return it != m_Values.end() ? &*it : nullptr;
	}

	void SaveData::Set(const std::string& key, nlohmann::json value)
	{
		BS_CORE_ASSERT(!key.empty(), "save data keys must not be empty");
		m_Values[key] = std::move(value);
	}

	bool SaveData::Remove(const std::string& key)
	{
		return m_Values.erase(key) > 0;
	}

	void SaveData::Clear()
	{
		m_Values = nlohmann::json::object();
	}

	std::vector<std::string> SaveData::GetKeys() const
	{
		std::vector<std::string> keys;
		keys.reserve(m_Values.size());
		for (const auto& [key, value] : m_Values.items())
			keys.push_back(key);
		// nlohmann's object is a std::map, so this is already sorted; sort anyway to not depend on it.
		std::sort(keys.begin(), keys.end());
		return keys;
	}

}
