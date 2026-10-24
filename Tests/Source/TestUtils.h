#pragma once

#include <Basalt/Core/FileSystem.h>
#include <Basalt/Core/Input.h>
#include <Basalt/Core/Log.h>
#include <Basalt/Core/Platform.h>
#include <Basalt/Project/Project.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/Scripting/ScriptEngine.h>
#include <Basalt/UI/GameUI.h>

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace BasaltTest {

	// Fixed frame time of the tests (60 Hz).
	inline constexpr float Step = 1.0f / 60.0f;

	// Scratch root of this test run: <temp>/BasaltTests/<run>, unique per process, so test runs at the same time
	// (e.g. Debug and ASan builds) never share or delete each other's files.
	inline const std::filesystem::path& GetRunDirectory()
	{
		static const std::filesystem::path directory = std::filesystem::temp_directory_path() / "BasaltTests" / ("Run-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		return directory;
	}

	// Whether a directory holds a leftover temporary file of FileSystem's atomic writes (*.tmp).
	inline bool HasTempFiles(const std::filesystem::path& directory)
	{
		REQUIRE_MESSAGE(std::filesystem::is_directory(directory), "not a directory: ", directory.string());
		std::error_code error;
		for (const auto& entry : std::filesystem::directory_iterator(directory, error))
		{
			if (entry.path().extension() == ".tmp")
				return true;
		}
		return false;
	}

	// A fresh, empty temporary directory (<run directory>/<name>), removed on scope exit.
	class TempDirectory
	{
	public:
		explicit TempDirectory(const std::string& name)
			: m_Path(GetRunDirectory() / name)
		{
			std::error_code error;
			std::filesystem::remove_all(m_Path, error);
			std::filesystem::create_directories(m_Path, error);
			REQUIRE_MESSAGE(!error, "cannot create '", m_Path.string(), "': ", error.message());
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
		// With no value, the variable is removed for the scope.
		ScopedEnvVar(std::string name, const std::optional<std::string>& value)
			: m_Name(std::move(name))
			, m_Previous(Basalt::Platform::GetEnvVar(m_Name.c_str()))
		{
			const bool changed = value ? Basalt::Platform::SetEnvVar(m_Name.c_str(), *value) : Basalt::Platform::UnsetEnvVar(m_Name.c_str());
			REQUIRE_MESSAGE(changed, "cannot change ", m_Name);
		}
		~ScopedEnvVar()
		{
			const bool restored = m_Previous ? Basalt::Platform::SetEnvVar(m_Name.c_str(), *m_Previous) : Basalt::Platform::UnsetEnvVar(m_Name.c_str());
			CHECK_MESSAGE(restored, "cannot restore ", m_Name);
		}

		ScopedEnvVar(const ScopedEnvVar&) = delete;
		ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

	private:
		std::string m_Name;
		std::optional<std::string> m_Previous;
	};

	// Starts and ends a test with nothing held, no injected gamepads, the headless host view (with full game routing)
	// and the Normal cursor, so a test that changes the view, the routing or the cursor cannot leak it into the next one,
	// even when it fails half way.
	class ScopedGameInput
	{
	public:
		ScopedGameInput() { Restore(); }
		~ScopedGameInput() { Restore(); }

		ScopedGameInput(const ScopedGameInput&) = delete;
		ScopedGameInput& operator=(const ScopedGameInput&) = delete;

	private:
		static void Restore()
		{
			Basalt::GameUI::SetHostState({});
			Basalt::Input::SetCursorMode(Basalt::CursorMode::Normal);
			Basalt::Input::Reset();
			Basalt::Input::ForgetInjectedGamepads();
			Basalt::Input::EndFrame();
		}
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
			: m_Directory(name)
		{
			std::string error;
			m_Project = Basalt::Project::Create(m_Directory.GetPath(), name, error);
			Basalt::Project::SetActive(m_Project);
		}

		~TempProject() { Basalt::Project::SetActive(nullptr); }

		TempProject(const TempProject&) = delete;
		TempProject& operator=(const TempProject&) = delete;

		// Writes a project-relative file and returns its project-relative path.
		std::string WriteFile(const std::string& relativePath, const std::string& contents) const
		{
			Basalt::FileSystem::WriteTextFile(m_Directory.GetPath() / relativePath, contents);
			return relativePath;
		}

		const std::filesystem::path& GetDirectory() const { return m_Directory.GetPath(); }
		bool IsValid() const { return m_Project != nullptr; }

	private:
		TempDirectory m_Directory;
		Basalt::Ref<Basalt::Project> m_Project;
	};

	// macOS exports bundle the Vulkan loader and MoltenVK, which tests never load: stand-ins in <project>/FakeVulkanSDK,
	// found through VULKAN_SDK for the scope. Nothing on other platforms.
	class ScopedFakeVulkanSdk
	{
	public:
		explicit ScopedFakeVulkanSdk(const TempProject& project)
		{
#if defined(__APPLE__)
			project.WriteFile("FakeVulkanSDK/lib/libvulkan.1.dylib", "stand-in");
			project.WriteFile("FakeVulkanSDK/lib/libMoltenVK.dylib", "stand-in");
			m_Variable.emplace("VULKAN_SDK", (project.GetDirectory() / "FakeVulkanSDK").string());
#else
			(void)project;
#endif
		}

	private:
		std::optional<ScopedEnvVar> m_Variable;
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
