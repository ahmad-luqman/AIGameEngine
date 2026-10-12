#pragma once

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Core/Platform.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/Scripting/ScriptEngine.h>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace BasaltTest {

	// A fresh, empty temporary directory (BasaltTests/<name>), removed on scope exit.
	class TempDirectory
	{
	public:
		explicit TempDirectory(const std::string& name)
			: m_Path(std::filesystem::temp_directory_path() / "BasaltTests" / name)
		{
			std::error_code error;
			std::filesystem::remove_all(m_Path, error);
			std::filesystem::create_directories(m_Path, error);
		}
		~TempDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(m_Path, error);
		}

		TempDirectory(const TempDirectory&) = delete;
		TempDirectory& operator=(const TempDirectory&) = delete;

		const std::filesystem::path& GetPath() const { return m_Path; }

	private:
		std::filesystem::path m_Path;
	};

	// Sets an environment variable for the scope and restores its previous value (or absence) afterwards.
	class ScopedEnvVar
	{
	public:
		ScopedEnvVar(const char* name, const std::string& value)
			: m_Name(name)
			, m_Previous(Basalt::Platform::GetEnvVar(name))
		{
			Basalt::Platform::SetEnvVar(name, value);
		}
		~ScopedEnvVar()
		{
			if (m_Previous)
				Basalt::Platform::SetEnvVar(m_Name, *m_Previous);
			else
				Basalt::Platform::UnsetEnvVar(m_Name);
		}

		ScopedEnvVar(const ScopedEnvVar&) = delete;
		ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

	private:
		const char* m_Name;
		std::optional<std::string> m_Previous;
	};

	// The result of a Lua chunk in the engine (see ScriptEngine::ExecuteString), or "error: <message>".
	inline std::string Eval(Basalt::ScriptEngine& engine, const std::string& code)
	{
		std::string result;
		if (!engine.ExecuteString(code, result))
			return "error: " + result;
		return result;
	}

	inline std::string Eval(Basalt::Scene& scene, const std::string& code)
	{
		return Eval(*scene.GetScriptEngine(), code);
	}

	// Every snippet must fail with a message containing its expected fragment.
	inline void CheckLuaErrors(Basalt::ScriptEngine& engine, std::span<const std::pair<const char*, const char*>> cases)
	{
		std::string result;
		for (const auto& [code, expected] : cases)
		{
			CHECK_FALSE(engine.ExecuteString(code, result));
			CHECK_MESSAGE(result.find(expected) != std::string::npos, code, " -> ", result);
		}
	}

	// Creates an empty, active project in a fresh temporary directory; removed and deactivated on scope exit.
	class TempProject
	{
	public:
		explicit TempProject(const std::string& name)
		{
			m_Directory = std::filesystem::temp_directory_path() / "BasaltTests" / name;
			std::filesystem::remove_all(m_Directory);
			std::string error;
			m_Project = Basalt::Project::Create(m_Directory, name, error);
			Basalt::Project::SetActive(m_Project);
		}

		~TempProject()
		{
			Basalt::Project::SetActive(nullptr);
			std::error_code error;
			std::filesystem::remove_all(m_Directory, error);
		}

		TempProject(const TempProject&) = delete;
		TempProject& operator=(const TempProject&) = delete;

		// Writes a project-relative file and returns its project-relative path.
		std::string WriteFile(const std::string& relativePath, const std::string& contents) const
		{
			Basalt::FileSystem::WriteTextFile(m_Directory / relativePath, contents);
			return relativePath;
		}

		const std::filesystem::path& GetDirectory() const { return m_Directory; }
		bool IsValid() const { return m_Project != nullptr; }

	private:
		std::filesystem::path m_Directory;
		Basalt::Ref<Basalt::Project> m_Project;
	};

	// Log messages containing `text` since the history's total count was `since`.
	inline int CountLogMessages(uint64_t since, const std::string& text)
	{
		uint64_t next = 0;
		int count = 0;
		for (const Basalt::LogMessage& message : Basalt::Log::GetHistory().GetMessagesSince(since, next))
		{
			if (message.Text.find(text) != std::string::npos)
				count++;
		}
		return count;
	}

	// Standard base64 (with padding), for data: URIs in generated glTF files.
	inline std::string Base64(const void* data, size_t size)
	{
		static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		const auto* bytes = static_cast<const uint8_t*>(data);
		std::string out;
		for (size_t i = 0; i < size; i += 3)
		{
			const uint32_t chunk = (bytes[i] << 16) | ((i + 1 < size ? bytes[i + 1] : 0) << 8) | (i + 2 < size ? bytes[i + 2] : 0);
			out += alphabet[(chunk >> 18) & 63];
			out += alphabet[(chunk >> 12) & 63];
			out += i + 1 < size ? alphabet[(chunk >> 6) & 63] : '=';
			out += i + 2 < size ? alphabet[chunk & 63] : '=';
		}
		return out;
	}

}
