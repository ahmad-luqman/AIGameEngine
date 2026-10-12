#include "Basalt/Project/SaveData.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <system_error>
#include <utility>

namespace Basalt {

	namespace {

		// A free backup name next to the save file, or nullopt when all are taken.
		std::optional<std::filesystem::path> FreeBackupPath(const std::filesystem::path& directory)
		{
			constexpr int MaxBackups = 1000;
			for (int index = 1; index <= MaxBackups; index++)
			{
				const std::string suffix = index == 1 ? std::string() : "." + std::to_string(index);
				std::filesystem::path candidate = directory / (std::string(SaveData::CorruptFileStem) + suffix + ".json");
				std::error_code error;
				if (!std::filesystem::exists(candidate, error) && !error)
					return candidate;
			}
			return std::nullopt;
		}

	}

	SaveData::SaveData(std::filesystem::path path)
		: m_Path(std::move(path))
	{
	}

	std::optional<std::filesystem::path> SaveData::GetDefaultPath(const std::string& projectName, std::string& outError)
	{
		const std::optional<std::filesystem::path> base = Platform::GetUserDataDirectory(outError);
		if (!base)
			return std::nullopt;
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

		// Device names are reserved on Windows with any extension ("NUL.txt" too), so the '_' goes after the stem.
		static constexpr std::array<std::string_view, 24> Reserved = { "CON", "PRN", "AUX", "NUL", "COM0", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT0", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
		const size_t stemLength = std::min(result.find('.'), result.size());
		std::string stem = result.substr(0, stemLength);
		std::transform(stem.begin(), stem.end(), stem.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
		if (std::find(Reserved.begin(), Reserved.end(), stem) != Reserved.end())
			result.insert(stemLength, 1, '_');
		return result;
	}

	bool SaveData::Load(std::string& outError)
	{
		const std::string where = "save data '" + m_Path.string() + "'";
		std::error_code error;
		const bool exists = std::filesystem::exists(m_Path, error);
		if (error)
		{
			outError = "cannot access " + where + ": " + error.message();
			m_SaveBlocked = outError;
			return false;
		}
		if (!exists)
		{
			m_Values = nlohmann::json::object();
			m_SaveBlocked.clear();
			return true;
		}

		const uintmax_t size = std::filesystem::file_size(m_Path, error);
		if (error)
		{
			outError = "cannot read " + where + ": " + error.message();
			m_SaveBlocked = outError;
			return false;
		}

		std::string problem;
		if (size > MaxFileSize)
		{
			problem = "it is larger than " + std::to_string(MaxFileSize / (size_t{ 1024 } * 1024)) + " MB";
		}
		else
		{
			errno = 0;
			const std::optional<std::string> text = FileSystem::ReadTextFile(m_Path);
			if (!text)
			{
				const int reason = errno;
				outError = "cannot read " + where + (reason != 0 ? ": " + std::generic_category().message(reason) : std::string());
				m_SaveBlocked = outError;
				return false;
			}
			nlohmann::json values;
			switch (Parse(*text, values, problem))
			{
				case ParseStatus::Valid:
					m_Values = std::move(values);
					m_SaveBlocked.clear();
					return true;
				case ParseStatus::Newer:
					outError = where + " was written by a newer version of the game (" + problem + "); it was left untouched and will not be overwritten";
					m_SaveBlocked = outError;
					return false;
				case ParseStatus::Malformed:
					break;
			}
		}

		// Malformed: keep it for recovery, then start afresh (the next Save writes a new file).
		outError = where + " is unreadable (" + problem + ")";
		const std::optional<std::filesystem::path> backup = FreeBackupPath(m_Path.parent_path());
		if (backup)
			std::filesystem::rename(m_Path, *backup, error);
		if (!backup || error)
		{
			outError += "; moving it aside failed (" + (backup ? error.message() : std::string("too many backups")) + "), so it will not be overwritten";
			m_SaveBlocked = outError;
			return false;
		}
		outError += "; it was moved to '" + backup->string() + "' and the game starts with empty save data";
		m_Values = nlohmann::json::object();
		m_SaveBlocked.clear();
		return false;
	}

	bool SaveData::Save(std::string& outError) const
	{
		if (!m_SaveBlocked.empty())
		{
			outError = "not saving: " + m_SaveBlocked + " (Storage.Load() retries)";
			return false;
		}
		const std::string text = SaveToString();
		if (text.size() > MaxFileSize)
		{
			outError = "save data would be " + std::to_string(text.size() / 1024) + " KB, more than the " + std::to_string(MaxFileSize / (size_t{ 1024 } * 1024)) + " MB limit";
			return false;
		}
		std::error_code error;
		std::filesystem::create_directories(m_Path.parent_path(), error);
		if (error)
		{
			outError = "cannot create the save data directory '" + m_Path.parent_path().string() + "': " + error.message();
			return false;
		}
		if (!FileSystem::WriteTextFile(m_Path, text))
		{
			outError = "cannot write save data '" + m_Path.string() + "' (the log has the reason)";
			return false;
		}
		return true;
	}

	bool SaveData::LoadFromString(std::string_view text, std::string& outError)
	{
		nlohmann::json values;
		if (Parse(text, values, outError) != ParseStatus::Valid)
			return false;
		m_Values = std::move(values);
		return true;
	}

	SaveData::ParseStatus SaveData::Parse(std::string_view text, nlohmann::json& outValues, std::string& outError)
	{
		const nlohmann::json data = ParseJson(text);
		if (data.is_discarded())
		{
			outError = "not valid JSON";
			return ParseStatus::Malformed;
		}
		if (!data.is_object())
		{
			outError = "the file must contain a JSON object";
			return ParseStatus::Malformed;
		}
		for (const auto& [key, value] : data.items())
		{
			if (key != "Format" && key != "Version" && key != "Values")
			{
				outError = "unknown key '" + key + "' (valid: Format, Version, Values)";
				return ParseStatus::Malformed;
			}
		}

		const auto format = data.find("Format");
		if (format == data.end() || !format->is_string() || *format != "BasaltSaveData")
		{
			outError = "'Format' must be \"BasaltSaveData\"";
			return ParseStatus::Malformed;
		}
		const auto version = data.find("Version");
		const int64_t versionNumber = version != data.end() && version->is_number_integer() ? version->get<int64_t>() : 0;
		if (versionNumber < 1)
		{
			outError = "'Version' must be a positive integer";
			return ParseStatus::Malformed;
		}
		if (versionNumber > FormatVersion)
		{
			outError = "version " + std::to_string(versionNumber) + ", newer than supported version " + std::to_string(FormatVersion);
			return ParseStatus::Newer;
		}
		const auto values = data.find("Values");
		if (values == data.end() || !values->is_object())
		{
			outError = "'Values' must be an object";
			return ParseStatus::Malformed;
		}
		nlohmann::json result = nlohmann::json::object();
		for (const auto& [key, value] : values->items())
		{
			if (key.empty())
			{
				outError = "'Values' has an empty key";
				return ParseStatus::Malformed;
			}
			// A null value (a hand edit) means no value.
			if (!value.is_null())
				result[key] = value;
		}
		outValues = std::move(result);
		return ParseStatus::Valid;
	}

	std::string SaveData::SaveToString() const
	{
		const nlohmann::json data = { { "Format", "BasaltSaveData" }, { "Version", FormatVersion }, { "Values", m_Values } };
		// Storage.Set only stores valid UTF-8; replacing (rather than throwing) keeps a save working regardless.
		return data.dump(1, '\t', false, nlohmann::json::error_handler_t::replace);
	}

	const nlohmann::json* SaveData::Find(const std::string& key) const
	{
		const auto it = m_Values.find(key);
		return it != m_Values.end() ? &*it : nullptr;
	}

	bool SaveData::Set(const std::string& key, nlohmann::json value)
	{
		if (key.empty())
			return false;
		m_Values[key] = std::move(value);
		return true;
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
		// nlohmann::json objects are std::maps, so the keys come sorted.
		std::vector<std::string> keys;
		keys.reserve(m_Values.size());
		for (const auto& [key, value] : m_Values.items())
			keys.push_back(key);
		return keys;
	}

}
