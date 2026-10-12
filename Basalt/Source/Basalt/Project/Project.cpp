#include "Basalt/Project/Project.h"

#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Renderer/RenderMath.h"

#include <nlohmann/json.hpp>

#include <system_error>

namespace Basalt {

	namespace {

		// Reads an optional number in [min, max] into `value`; false (with outError) when the key is present but
		// not such a number.
		bool ReadBoundedFloat(const nlohmann::json& data, const char* key, float min, float max, float& value, const std::filesystem::path& filePath, std::string& outError)
		{
			const auto it = data.find(key);
			if (it == data.end())
				return true;
			if (JsonToFloat(*it, value) && value >= min && value <= max)
				return true;
			outError = fmt::format("invalid project file '{}': {} must be a number in [{}, {}]", filePath.string(), key, min, max);
			return false;
		}

	}

	Ref<Project> Project::s_ActiveProject;

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
		if (const auto dynamic = data.find("DynamicResolution"); dynamic != data.end())
		{
			// Named explicitly: nlohmann's type error would not say which key is wrong.
			if (!dynamic->is_boolean())
			{
				outError = fmt::format("invalid project file '{}': DynamicResolution must be true or false (TargetFrameRate and MinRenderScale are separate keys)", filePath.string());
				return nullptr;
			}
			config.DynamicResolution.Enabled = dynamic->get<bool>();
		}
		if (!ReadBoundedFloat(data, "RenderScale", MinRenderScale, MaxRenderScale, config.RenderScale, filePath, outError) ||
			!ReadBoundedFloat(data, "TargetFrameRate", MinTargetFrameRate, MaxTargetFrameRate, config.DynamicResolution.TargetFrameRate, filePath, outError) ||
			!ReadBoundedFloat(data, "MinRenderScale", MinRenderScale, MaxRenderScale, config.DynamicResolution.MinScale, filePath, outError))
			return nullptr;
		if (const auto seed = data.find("RandomSeed"); seed != data.end())
		{
			// Any 32-bit unsigned integer; nlohmann's get<uint32_t> would wrap negative or larger values.
			if (!seed->is_number_integer() || seed->get<int64_t>() < 0 || seed->get<int64_t>() > static_cast<int64_t>(UINT32_MAX))
			{
				outError = fmt::format("invalid project file '{}': RandomSeed must be an integer between 0 and {}", filePath.string(), UINT32_MAX);
				return nullptr;
			}
			config.RandomSeed = static_cast<uint32_t>(seed->get<int64_t>());
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
			{ "RenderScale", m_Config.RenderScale },
			{ "DynamicResolution", m_Config.DynamicResolution.Enabled },
			{ "TargetFrameRate", m_Config.DynamicResolution.TargetFrameRate },
			{ "MinRenderScale", m_Config.DynamicResolution.MinScale },
			{ "PhysicsLayers", m_Config.Physics.ToJson() },
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
