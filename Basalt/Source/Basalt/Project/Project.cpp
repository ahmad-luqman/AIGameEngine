#include "Basalt/Project/Project.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Renderer/RenderMath.h"

#include <nlohmann/json.hpp>

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

		for (const char* sub : { "Assets", "Assets/Scenes", "Assets/Scripts", "Assets/Prefabs", "Assets/Models", "Assets/Textures", "Assets/Audio", "Assets/Environments" })
		{
			std::filesystem::create_directories(absolute / sub, error);
			if (error)
			{
				outError = "cannot create '" + (absolute / sub).string() + "': " + error.message();
				return nullptr;
			}
		}

		Ref<Project> project = CreateRef<Project>();
		project->m_Directory = absolute;
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
		ProjectConfig& config = project->m_Config;
		try
		{
			config.Name = data.value("Name", config.Name);
			config.StartScene = data.value("StartScene", config.StartScene);
			config.WindowWidth = data.value("WindowWidth", config.WindowWidth);
			config.WindowHeight = data.value("WindowHeight", config.WindowHeight);
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
		return project;
	}

	bool Project::Save(std::string& outError) const
	{
		const nlohmann::json data = {
			{ "Format", "BasaltProject" },
			{ "Name", m_Config.Name },
			{ "StartScene", m_Config.StartScene },
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
		if (!FileSystem::WriteTextFile(GetProjectFilePath(), data.dump(1, '\t')))
		{
			outError = "cannot write '" + GetProjectFilePath().string() + "'";
			return false;
		}
		return true;
	}

	std::filesystem::path Project::ResolvePath(const std::filesystem::path& relativePath)
	{
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
