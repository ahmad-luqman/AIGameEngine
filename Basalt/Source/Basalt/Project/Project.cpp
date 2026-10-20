#include "Basalt/Project/Project.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Renderer/RenderMath.h"

#include <nlohmann/json.hpp>

#include <sstream>
#include <system_error>

namespace Basalt {

	Ref<Project> Project::s_ActiveProject;

	GraphicsSettings Project::GetActiveGraphicsSettings()
	{
		return s_ActiveProject ? s_ActiveProject->m_Config.Graphics : GraphicsSettings{};
	}

	Ref<Project> Project::Create(const std::filesystem::path& directory, const std::string& name, std::string& outError)
	{
		std::error_code error;
		const std::filesystem::path absolute = std::filesystem::absolute(directory, error);
		if (error)
		{
			outError = "invalid project directory '" + directory.string() + "'";
			return nullptr;
		}
		if (FileSystem::Exists(absolute / FileName))
		{
			outError = "a project already exists in '" + absolute.string() + "'";
			return nullptr;
		}
		// A new project has no actions, so Save writes no input file: one left there would be loaded as its own.
		if (FileSystem::Exists(absolute / InputFileName))
		{
			outError = "'" + (absolute / InputFileName).string() + "' already exists (move it away to create a project here)";
			return nullptr;
		}

		for (const char* sub : { "Assets", "Assets/Scenes", "Assets/Scripts", "Assets/Prefabs", "Assets/Models", "Assets/Textures", "Assets/Audio", "Assets/Environments" })
		{
			std::filesystem::create_directories(absolute / sub, error);
			if (error)
			{
				outError = "cannot create '" + (absolute / sub).string() + "': " + error.message();
				return nullptr;
			}
		}

		// Editor caches (thumbnails) are rebuilt from the assets and never belong in version control; keep an existing
		// ignore file (a fresh repository's, say) and add the rule.
		const std::filesystem::path ignorePath = absolute / ".gitignore";
		const auto existing = FileSystem::ReadTextFile(ignorePath);
		if (!existing && FileSystem::Exists(ignorePath))
		{
			outError = "cannot read '" + ignorePath.string() + "' to add the Cache/ rule";
			return nullptr;
		}
		std::string ignore = existing.value_or("");
		const auto ignoresCache = [](const std::string& text) {
			std::istringstream lines(text);
			for (std::string line; std::getline(lines, line);)
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line == "Cache/" || line == "Cache" || line == "/Cache/" || line == "/Cache")
					return true;
			}
			return false;
		};
		if (!ignoresCache(ignore))
		{
			if (!ignore.empty() && ignore.back() != '\n')
				ignore += '\n';
			if (!FileSystem::WriteTextFile(ignorePath, ignore + "Cache/\n", outError))
				return nullptr;
		}

		Ref<Project> project = CreateRef<Project>();
		project->m_Directory = absolute;
		project->m_Assets = AssetRegistry(absolute);
		project->m_Config.Name = name;
		if (!project->Save(outError))
			return nullptr;
		return project;
	}

	Ref<Project> Project::Load(const std::filesystem::path& path, std::string& outError)
	{
		std::error_code error;
		std::filesystem::path filePath = std::filesystem::absolute(path, error);
		if (std::filesystem::is_directory(filePath, error))
			filePath /= FileName;

		const auto text = FileSystem::ReadTextFile(filePath);
		if (!text)
		{
			outError = "cannot read project file '" + filePath.string() + "'";
			return nullptr;
		}

		const nlohmann::json data = ParseJson(*text);
		if (data.is_discarded() || !data.is_object())
		{
			outError = "'" + filePath.string() + "' is not valid JSON";
			return nullptr;
		}

		Ref<Project> project = CreateRef<Project>();
		project->m_Directory = filePath.parent_path();
		project->m_Assets = AssetRegistry(project->m_Directory);
		if (!project->m_Assets.Refresh(outError))
		{
			BS_CORE_ERROR("Project metadata: {}", outError);
			outError.clear();
		}
		for (const auto& [asset, problem] : project->m_Assets.GetProblems())
			BS_CORE_ERROR("Asset metadata {}.meta: {}", asset, problem);
		ProjectConfig& config = project->m_Config;
		try
		{
			config.Name = data.value("Name", config.Name);
			if (data.contains("StartScene") && !project->m_Assets.Decode(data["StartScene"], config.StartScene, outError))
			{
				const auto& reference = data["StartScene"];
				if (!reference.is_object() || reference.size() != 2 || !reference.contains("Path") || !reference["Path"].is_string() || !reference.contains("UUID") || !reference["UUID"].is_string() || !AssetRegistry::ParseID(reference["UUID"].get_ref<const std::string&>(), outError))
					return nullptr;
				project->m_UnresolvedStartScene = reference;
				project->m_StartSceneError = outError;
				BS_CORE_ERROR("Project: {}", outError);
				config.StartScene.clear();
				outError.clear();
			}
			config.Fullscreen = data.value("Fullscreen", config.Fullscreen);
		}
		catch (const nlohmann::json::exception& e)
		{
			outError = "invalid project file '" + filePath.string() + "': " + e.what();
			return nullptr;
		}
		nlohmann::json graphicsPatch = nlohmann::json::object();
		for (const char* key : { "VSync", "ShadowQuality", "SSAOQuality", "RenderScale", "DynamicResolution", "TargetFrameRate", "MinRenderScale" })
			if (data.contains(key))
				graphicsPatch[key] = data[key];
		if (!PatchGraphicsSettings(graphicsPatch, config.Graphics, outError))
		{
			outError = fmt::format("invalid project file '{}': {}", filePath.string(), outError);
			return nullptr;
		}
		if (const auto seed = data.find("RandomSeed"); seed != data.end() && !JsonToUInt32(*seed, config.RandomSeed))
		{
			outError = fmt::format("invalid project file '{}': RandomSeed must be an integer between 0 and {}", filePath.string(), UINT32_MAX);
			return nullptr;
		}
		// The window size reaches the runtime's window unchecked otherwise (a negative value would wrap around).
		for (const auto& [key, size] : { std::pair{ "WindowWidth", &config.WindowWidth }, std::pair{ "WindowHeight", &config.WindowHeight } })
		{
			const auto value = data.find(key);
			if (value != data.end() && (!JsonToUInt32(*value, *size) || *size < ProjectConfig::MinWindowSize || *size > ProjectConfig::MaxWindowSize))
			{
				outError = fmt::format("invalid project file '{}': {} must be an integer in [{}, {}]", filePath.string(), key, ProjectConfig::MinWindowSize, ProjectConfig::MaxWindowSize);
				return nullptr;
			}
		}
		if (const auto step = data.find("FixedTimestep"); step != data.end() && (!JsonToFloat(*step, config.FixedTimestep) || !IsValidFixedTimestep(config.FixedTimestep)))
		{
			outError = fmt::format("invalid project file '{}': FixedTimestep must be a number of seconds in (0, {}]", filePath.string(), MaxFixedTimestep);
			return nullptr;
		}
		if (const auto layers = data.find("PhysicsLayers"); layers != data.end())
		{
			std::string layerError;
			auto physics = PhysicsLayers::FromJson(*layers, layerError);
			if (!physics)
			{
				outError = "invalid project file '" + filePath.string() + "': " + layerError;
				return nullptr;
			}
			config.Physics = std::move(*physics);
		}
		auto actions = ReadInputActions(project->GetInputFilePath(), outError);
		if (!actions)
			return nullptr;
		config.InputActions = std::move(*actions);
		project->m_SavedInputActions = config.InputActions;
		return project;
	}

	std::optional<InputActionMap> Project::ReadInputActions(const std::filesystem::path& path, std::string& outError)
	{
		// Only a file that is not there means "no actions"; one that cannot be checked is an error, not an empty map.
		std::error_code statusError;
		const std::filesystem::file_status status = std::filesystem::status(path, statusError);
		if (status.type() == std::filesystem::file_type::not_found)
			return InputActionMap{};
		std::string reason;
		if (statusError)
			reason = ": " + statusError.message();
		else if (status.type() != std::filesystem::file_type::regular)
			reason = ": it is not a file";
		const auto text = reason.empty() ? FileSystem::ReadTextFile(path) : std::nullopt;
		if (!text)
		{
			outError = "cannot read input actions file '" + path.string() + "'" + reason;
			return std::nullopt;
		}
		std::string error;
		const nlohmann::json data = ParseJson(*text, error);
		auto actions = data.is_discarded() ? std::nullopt : InputActionMap::FromJson(data, error);
		if (!actions)
		{
			outError = "invalid input actions file '" + path.string() + "': " + error;
			return std::nullopt;
		}
		return actions;
	}

	void Project::SetStartScene(std::string path)
	{
		m_Config.StartScene = std::move(path);
		m_UnresolvedStartScene = nullptr;
		m_StartSceneError.clear();
	}

	bool Project::Save(std::string& outError, bool replaceUnresolvedStartScene)
	{
		nlohmann::json startScene;
		if (m_Config.StartScene.empty() && !m_UnresolvedStartScene.is_null() && !replaceUnresolvedStartScene)
			startScene = m_UnresolvedStartScene;
		else if ((!m_Config.StartScene.empty() && !m_Assets.Refresh(outError)) || !m_Assets.Encode(m_Config.StartScene, startScene, outError))
			return false;

		const bool writeInput = m_Config.InputActions != m_SavedInputActions;
		const bool inputExisted = FileSystem::Exists(GetInputFilePath());
		// The file as it is now (it may have been edited by hand since it was loaded), to put back exactly. Plain values
		// rather than an optional: GCC's -Wmaybe-uninitialized misfires on optional conditionals in Release.
		std::string previousInput;
		bool previousInputRead = false;
		if (writeInput && inputExisted)
		{
			if (auto text = FileSystem::ReadTextFile(GetInputFilePath()))
			{
				previousInput = std::move(*text);
				previousInputRead = true;
			}
		}
		if (writeInput && !FileSystem::WriteTextFile(GetInputFilePath(), m_Config.InputActions.ToJson().dump(1, '\t'), outError))
			return false;
		const nlohmann::json data = {
			{ "Format", "BasaltProject" },
			{ "Name", m_Config.Name },
			{ "StartScene", startScene },
			{ "WindowWidth", m_Config.WindowWidth },
			{ "WindowHeight", m_Config.WindowHeight },
			{ "Fullscreen", m_Config.Fullscreen },
			{ "VSync", m_Config.Graphics.VSync },
			{ "ShadowQuality", GraphicsQualityName(m_Config.Graphics.ShadowQuality) },
			{ "SSAOQuality", GraphicsQualityName(m_Config.Graphics.SSAOQuality) },
			{ "RenderScale", m_Config.Graphics.RenderScale },
			{ "DynamicResolution", m_Config.Graphics.DynamicResolution.Enabled },
			{ "TargetFrameRate", m_Config.Graphics.DynamicResolution.TargetFrameRate },
			{ "MinRenderScale", m_Config.Graphics.DynamicResolution.MinScale },
			{ "PhysicsLayers", m_Config.Physics.ToJson() },
			{ "FixedTimestep", m_Config.FixedTimestep },
			{ "RandomSeed", m_Config.RandomSeed },
		};
		if (!FileSystem::WriteTextFile(GetProjectFilePath(), data.dump(1, '\t'), outError))
		{
			// Neither file changes then: put back the input actions written above (or remove the file they created).
			if (!writeInput)
				return false;
			std::string restoreError;
			std::error_code removeError;
			if (inputExisted)
				FileSystem::WriteTextFile(GetInputFilePath(), previousInputRead ? previousInput : m_SavedInputActions.ToJson().dump(1, '\t'), restoreError);
			else if (!std::filesystem::remove(GetInputFilePath(), removeError) && removeError)
				restoreError = removeError.message();
			if (!restoreError.empty())
				outError += "; " + GetInputFilePath().string() + " was already updated and could not be put back: " + restoreError;
			return false;
		}
		if (writeInput)
			m_SavedInputActions = m_Config.InputActions;
		if (replaceUnresolvedStartScene || !m_Config.StartScene.empty())
		{
			m_UnresolvedStartScene = nullptr;
			m_StartSceneError.clear();
		}
		return true;
	}

	std::filesystem::path Project::ResolvePath(const std::filesystem::path& relativePath)
	{
		// Unresolved identity keys must never be interpreted as ordinary files, even if one mimics the key.
		if (FileSystem::PathToGenericUtf8(relativePath).starts_with("asset-uuid:"))
			return {};
		if (relativePath.is_absolute())
			return relativePath;
		if (s_ActiveProject)
			return s_ActiveProject->m_Directory / relativePath;
		std::error_code error;
		return std::filesystem::absolute(relativePath, error);
	}

	std::string Project::MakeRelative(const std::filesystem::path& absolutePath)
	{
		if (!s_ActiveProject)
			return absolutePath.generic_string();
		std::error_code error;
		const std::filesystem::path relative = std::filesystem::relative(absolutePath, s_ActiveProject->m_Directory, error);
		if (error || relative.empty() || *relative.begin() == "..")
			return absolutePath.generic_string();
		return relative.generic_string();
	}

}
