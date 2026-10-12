#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Math/Random.h"
#include "Basalt/Physics/PhysicsLayers.h"
#include "Basalt/Renderer/DynamicResolution.h"

#include <filesystem>
#include <string>

namespace Basalt {

	struct ProjectConfig
	{
		std::string Name = "Untitled";
		// Scene opened by the editor and played first by the runtime (project-relative path).
		std::string StartScene;
		// Window settings used by the exported game.
		uint32_t WindowWidth = 1600;
		uint32_t WindowHeight = 900;
		bool Fullscreen = false;
		// Fraction of the window resolution the scene renders at (MinRenderScale..MaxRenderScale); the
		// tonemap pass resamples it to the window. Below 1 trades sharpness for GPU time.
		float RenderScale = 1.0f;
		// Lowers the render scale (to no less than DynamicResolution.MinScale, the project key MinRenderScale)
		// when the GPU cannot hold the target frame rate; RenderScale is then the highest scale.
		DynamicResolutionSettings DynamicResolution;
		// Collision layers and matrix used by every scene of the project.
		PhysicsLayers Physics;
		// Seeds the gameplay random streams of play sessions (Lua's Math.Random/math.random, particle emitters;
		// not UUIDs of runtime spawns), so a session replays identically. Applies from the next play; changing
		// it changes the gameplay of scripts that use randomness.
		uint32_t RandomSeed = Random::DefaultSeed;
	};

	// A game project: a directory containing Project.bproject and an Assets/ folder. Asset references in
	// components are project-relative paths ("Assets/Models/Ship.gltf"). One project is active at a time.
	class Project
	{
	public:
		static constexpr const char* FileName = "Project.bproject";
		static constexpr const char* AssetDirectoryName = "Assets";

		// Creates the directory layout and project file. Fails if a project already exists there.
		static Ref<Project> Create(const std::filesystem::path& directory, const std::string& name, std::string& outError);
		// Accepts the project directory or the .bproject file itself.
		static Ref<Project> Load(const std::filesystem::path& path, std::string& outError);
		bool Save(std::string& outError) const;

		static void SetActive(const Ref<Project>& project) { s_ActiveProject = project; }
		static const Ref<Project>& GetActive() { return s_ActiveProject; }
		// The active project's RandomSeed, or Random::DefaultSeed without an active project (tests, tools).
		static uint32_t GetActiveRandomSeed() { return s_ActiveProject ? s_ActiveProject->m_Config.RandomSeed : Random::DefaultSeed; }

		// Resolves a project-relative path against the active project directory, or the working directory
		// when no project is active. Absolute paths are returned unchanged.
		static std::filesystem::path ResolvePath(const std::filesystem::path& relativePath);
		// Converts an absolute path inside the active project into a project-relative generic path.
		static std::string MakeRelative(const std::filesystem::path& absolutePath);

		ProjectConfig& GetConfig() { return m_Config; }
		const ProjectConfig& GetConfig() const { return m_Config; }
		const std::filesystem::path& GetDirectory() const { return m_Directory; }
		std::filesystem::path GetAssetDirectory() const { return m_Directory / AssetDirectoryName; }
		std::filesystem::path GetProjectFilePath() const { return m_Directory / FileName; }

	private:
		ProjectConfig m_Config;
		std::filesystem::path m_Directory;

		static Ref<Project> s_ActiveProject;
	};

}
