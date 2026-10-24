#pragma once

#include "Basalt/Asset/AssetRegistry.h"
#include "Basalt/Core/Base.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/InputActions.h"
#include "Basalt/Math/Random.h"
#include "Basalt/Physics/FixedTimestep.h"
#include "Basalt/Physics/PhysicsLayers.h"
#include "Basalt/Renderer/GraphicsSettings.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Basalt {

	// Saved project defaults shared by the editor, runtime and authoring tools.
	struct ProjectConfig
	{
		// Window sizes the runtime accepts (also the bounds of project.set windowWidth/windowHeight).
		static constexpr uint32_t MinWindowSize = 64;
		static constexpr uint32_t MaxWindowSize = 16384;

		static constexpr const char* DefaultVersion = "1.0.0";
		// Longest Name and Company in bytes: both go into length-limited package metadata (a Windows version resource's
		// lengths are 16-bit), and no title needs more.
		static constexpr size_t MaxTextLength = 256;

		std::string Name = "Untitled";
		// VersionFormat (ParseProjectVersion): Windows version resources hold four 16-bit numbers and macOS bundle versions
		// dotted integers.
		std::string Version = DefaultVersion;
		// Publisher shown in the Windows version resource and the macOS copyright, and part of the bundle identifier.
		std::string Company;
		// Project-relative PNG under Assets/: the window icon and the icon of packaged exports. Empty for none. An asset
		// reference like StartScene (the project file stores its UUID and path), but one whose file is gone stays here as
		// an unresolved asset-uuid: key (AssetRegistry::IsUnresolvedKey) for validation and export to report.
		std::string Icon;
		// Scene opened by the editor and played first by the runtime (project-relative path).
		std::string StartScene;
		// Window settings used by the exported game.
		uint32_t WindowWidth = 1600;
		uint32_t WindowHeight = 900;
		bool Fullscreen = false;
		// Defaults copied into each Play session; the project file keeps these keys flat.
		GraphicsSettings Graphics;
		// Collision layers and matrix used by every scene of the project.
		PhysicsLayers Physics;
		// Named input actions (Project::InputFileName, next to the project file). Play sessions read a copy, which
		// scripts may rebind (Scene::GetInputActions).
		InputActionMap InputActions;
		// Physics step (seconds) of every scene that does not override it (PhysicsSettings::FixedTimestep).
		// Applies from the next Play or Simulate.
		float FixedTimestep = DefaultFixedTimestep;
		// Seeds the gameplay random streams of play sessions (Lua's Math.Random/math.random, particle emitters,
		// and runtime entity UUIDs), so a session replays identically. Applies from the next play; changing
		// it changes the gameplay of scripts that use randomness.
		uint32_t RandomSeed = Random::DefaultSeed;
	};

	// The form of ProjectConfig::Version, for messages.
	constexpr const char* VersionFormat = "MAJOR.MINOR.PATCH or MAJOR.MINOR.PATCH.BUILD, each 0-65535 without leading zeros";
	// The numbers of a ProjectConfig::Version (a missing BUILD is 0), or nullopt when it is not VersionFormat. Leading
	// zeros are refused so the text and the numbers packages derive from it always agree.
	std::optional<std::array<uint16_t, 4>> ParseProjectVersion(std::string_view version);
	// False, with "<Name|Company> must be at most N bytes", when Name or Company is longer than MaxTextLength. Every path
	// that sets them checks (Project::Create, Project::Load, project.set), and so does MakePackageInfo, whatever built the
	// config.
	bool ValidateProjectText(const ProjectConfig& config, std::string& outError);

	// A game project: a directory containing Project.bproject and an Assets/ folder. Asset references in
	// components are project-relative paths ("Assets/Models/Ship.gltf"). One project is active at a time.
	class Project
	{
	public:
		static constexpr const char* FileName = "Project.bproject";
		static constexpr const char* AssetDirectoryName = "Assets";
		// The input actions file. Optional: a project without one has no actions.
		static constexpr const char* InputFileName = "Input.binput";
		// An exported game's Assets packed into one archive (ExportOptions::Archive), next to the project file.
		static constexpr const char* ArchiveFileName = "Assets.bpak";

		// Creates the directory layout and project file. Fails if a project already exists there.
		static Ref<Project> Create(const std::filesystem::path& directory, const std::string& name, std::string& outError);
		// Accepts the project directory or the .bproject file itself.
		static Ref<Project> Load(const std::filesystem::path& path, std::string& outError);
		// Reads an input actions file: an empty map when it does not exist, nullopt (with the reason) when it cannot be
		// read or is invalid.
		static std::optional<InputActionMap> ReadInputActions(const std::filesystem::path& path, std::string& outError);
		// Writes the input actions file when the actions changed since they were loaded or last saved (so a hand edit
		// of the file is never overwritten by an unrelated change), then the project file. A failure is reported (with
		// the system's reason) and leaves both files as they were: the actions are put back when only the project file
		// could not be written.
		// May create the start scene's sidecar. A full metadata scan is required only for a nonempty start path.
		// Unresolved start references survive unrelated saves; the flag explicitly repairs or clears them.
		bool Save(std::string& outError, bool replaceUnresolvedStartScene = false);

		static void SetActive(const Ref<Project>& project) { s_ActiveProject = project; }
		static const Ref<Project>& GetActive() { return s_ActiveProject; }
		// Graphics defaults for Edit mode and new Play sessions, including when no project is active.
		static GraphicsSettings GetActiveGraphicsSettings();
		// The active project's settings, or nullptr without an active project. Valid until the active project
		// changes; do not keep it.
		static const ProjectConfig* GetActiveConfig() { return s_ActiveProject ? &s_ActiveProject->m_Config : nullptr; }
		// The active project's RandomSeed, or Random::DefaultSeed without an active project (tests, tools).
		static uint32_t GetActiveRandomSeed() { return s_ActiveProject ? s_ActiveProject->m_Config.RandomSeed : Random::DefaultSeed; }
		// The active project's input actions, or an empty map without an active project.
		static InputActionMap GetActiveInputActions() { return s_ActiveProject ? s_ActiveProject->m_Config.InputActions : InputActionMap{}; }
		// The active project's FixedTimestep, or DefaultFixedTimestep without an active project.
		static float GetActiveFixedTimestep() { return s_ActiveProject ? s_ActiveProject->m_Config.FixedTimestep : DefaultFixedTimestep; }

		// Resolves a project-relative path against the active project directory, or the working directory
		// when no project is active. Absolute paths are returned unchanged.
		static std::filesystem::path ResolvePath(const std::filesystem::path& relativePath);
		// Converts an absolute path inside the active project into a project-relative generic path.
		static std::string MakeRelative(const std::filesystem::path& absolutePath);

		AssetRegistry& GetAssets() { return m_Assets; }
		const AssetRegistry& GetAssets() const { return m_Assets; }
		// An unresolved start UUID leaves the project open for repair, without selecting its hint's file.
		const std::string& GetStartSceneError() const { return m_StartSceneError; }
		void SetStartScene(std::string path);

		ProjectConfig& GetConfig() { return m_Config; }
		const ProjectConfig& GetConfig() const { return m_Config; }
		const std::filesystem::path& GetDirectory() const { return m_Directory; }
		std::filesystem::path GetAssetDirectory() const { return m_Directory / AssetDirectoryName; }
		std::filesystem::path GetProjectFilePath() const { return m_Directory / FileName; }
		std::filesystem::path GetInputFilePath() const { return m_Directory / InputFileName; }
		// The icon file (ProjectConfig::Icon), or an empty path without an icon or when its file is gone (an unresolved key).
		std::filesystem::path GetIconPath() const
		{
			if (m_Config.Icon.empty() || AssetRegistry::IsUnresolvedKey(m_Config.Icon))
				return {};
			return m_Directory / FileSystem::PathFromUtf8(m_Config.Icon);
		}

	private:
		AssetRegistry m_Assets;
		nlohmann::json m_UnresolvedStartScene;
		std::string m_StartSceneError;
		ProjectConfig m_Config;
		// The actions as Input.binput holds them (Load, Save): Save writes the file only when they differ.
		InputActionMap m_SavedInputActions;
		std::filesystem::path m_Directory;

		static Ref<Project> s_ActiveProject;
	};

}
