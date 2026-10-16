#include "Basalt/Project/ProjectValidator.h"

#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/Components.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scene/SceneSerializer.h"
#include "Basalt/Scripting/ScriptEngine.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <system_error>

namespace Basalt {

	namespace {

		// File types components reference by path. A string ending in one of these is treated as an asset path.
		const std::set<std::string> s_AssetExtensions = { ".gltf", ".glb", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".wav", ".mp3", ".ogg", ".flac", ".ttf", ".otf", ".lua", ".bscene", ".bprefab" };

		std::string Lowercase(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		}

		// "Models/Ship.glb#image/0" names an image inside Ship.glb: the file is the part before '#'.
		std::string StripSubAsset(const std::string& reference)
		{
			return reference.substr(0, reference.find('#'));
		}

		bool LooksLikeAssetPath(const std::string& value)
		{
			if (value.rfind("builtin://", 0) == 0)
				return true;
			const std::string file = StripSubAsset(value);
			return s_AssetExtensions.contains(Lowercase(std::filesystem::path(file).extension().string()));
		}

		// Collects every string value of a component's JSON that looks like an asset path.
		void CollectAssetPaths(const nlohmann::json& value, std::set<std::string>& outPaths)
		{
			if (value.is_string())
			{
				if (const std::string& text = value.get_ref<const std::string&>(); LooksLikeAssetPath(text))
					outPaths.insert(text);
			}
			else if (value.is_structured())
			{
				for (const auto& element : value)
					CollectAssetPaths(element, outPaths);
			}
		}

		class Validator
		{
		public:
			explicit Validator(const Project& project)
				: m_Project(project)
				, m_Directory(project.GetDirectory())
			{
			}

			ValidationReport Run()
			{
				CheckProjectFile();
				CheckStartScene();
				std::vector<std::filesystem::path> scenes, prefabs, scripts;
				ListAssets(scenes, prefabs, scripts);
				for (const auto& path : scenes)
					CheckScene(path);
				for (const auto& path : prefabs)
					CheckPrefab(path);
				for (const auto& path : scripts)
					CheckScriptSyntax(path);
				for (const auto& [script, file] : m_ReferencedScripts)
					CheckScriptClass(script, file);
				return std::move(m_Report);
			}

		private:
			std::string Relative(const std::filesystem::path& path) const
			{
				return path.lexically_relative(m_Directory).generic_string();
			}

			void CheckProjectFile()
			{
				const ProjectConfig& config = m_Project.GetConfig();
				if (config.WindowWidth < 64 || config.WindowWidth > 16384 || config.WindowHeight < 64 || config.WindowHeight > 16384)
					m_Report.Add(ValidationSeverity::Error, Project::FileName, fmt::format("the window size {}x{} is outside [64, 16384]", config.WindowWidth, config.WindowHeight));
				const auto text = FileSystem::ReadTextFile(m_Project.GetProjectFilePath());
				const nlohmann::json data = text ? ParseJson(*text) : nlohmann::json();
				const auto format = data.is_object() ? data.find("Format") : data.end();
				if (format == data.end() || *format != "BasaltProject")
					m_Report.Add(ValidationSeverity::Warning, Project::FileName, "the file does not declare \"Format\": \"BasaltProject\"");
			}

			void CheckStartScene()
			{
				const std::string& start = m_Project.GetConfig().StartScene;
				if (start.empty())
				{
					m_Report.Add(ValidationSeverity::Error, Project::FileName, "no StartScene is set (project.set startScene)");
					return;
				}
				if (std::filesystem::path(start).extension() != ".bscene")
					m_Report.Add(ValidationSeverity::Error, Project::FileName, "the StartScene '" + start + "' is not a .bscene file");
				else
					CheckReference(start, Project::FileName, "StartScene");
			}

			void ListAssets(std::vector<std::filesystem::path>& scenes, std::vector<std::filesystem::path>& prefabs, std::vector<std::filesystem::path>& scripts)
			{
				std::error_code error;
				const std::filesystem::path assets = m_Project.GetAssetDirectory();
				if (!std::filesystem::is_directory(assets, error))
				{
					m_Report.Add(ValidationSeverity::Error, Project::AssetDirectoryName, "the Assets directory is missing");
					return;
				}
				for (auto it = std::filesystem::recursive_directory_iterator(assets, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
				{
					if (!it->is_regular_file(error))
						continue;
					const std::string extension = Lowercase(it->path().extension().string());
					if (extension == ".bscene")
						scenes.push_back(it->path());
					else if (extension == ".bprefab")
						prefabs.push_back(it->path());
					else if (extension == ".lua")
						scripts.push_back(it->path());
				}
				if (error)
					m_Report.Add(ValidationSeverity::Error, Project::AssetDirectoryName, "cannot list the assets: " + error.message());
				// Directory order differs between platforms; reports should not.
				for (auto* list : { &scenes, &prefabs, &scripts })
					std::sort(list->begin(), list->end());
			}

			void CheckScene(const std::filesystem::path& path)
			{
				std::string error;
				const Ref<Scene> scene = SceneSerializer::LoadScene(path, error);
				if (!scene)
				{
					m_Report.Add(ValidationSeverity::Error, Relative(path), "does not load: " + error);
					return;
				}
				CheckEntities(*scene, Relative(path));
			}

			void CheckPrefab(const std::filesystem::path& path)
			{
				Scene scene;
				std::string error;
				if (!SceneSerializer::InstantiatePrefab(scene, path, Relative(path), {}, error))
				{
					m_Report.Add(ValidationSeverity::Error, Relative(path), "does not load: " + error);
					return;
				}
				CheckEntities(scene, Relative(path));
			}

			void CheckEntities(Scene& scene, const std::string& file)
			{
				const nlohmann::json data = SceneSerializer::SerializeScene(scene);
				std::set<std::string> paths;
				for (const auto& entity : data["Entities"])
					if (const auto components = entity.find("Components"); components != entity.end())
						CollectAssetPaths(*components, paths);
				for (const std::string& path : paths)
					CheckReference(path, file, "references");
				for (auto handle : scene.GetAllEntitiesWith<ScriptComponent>())
				{
					const std::string& script = scene.GetRegistry().get<ScriptComponent>(handle).Script;
					if (!script.empty())
						m_ReferencedScripts.emplace(script, file);
				}
				for (const UnknownPhysicsLayerUse& use : FindUnknownPhysicsLayers(scene, m_Project.GetConfig().Physics))
					m_Report.Add(ValidationSeverity::Warning, file, fmt::format("'{}' uses the undefined physics layer '{}' (it plays on Default)", use.EntityName, use.Layer));
			}

			// A path a scene, prefab or the project refers to: it must name an existing file inside Assets/, spelled
			// with the file's exact letter case (the game may run on a case-sensitive file system).
			void CheckReference(const std::string& reference, const std::string& file, const char* role)
			{
				if (reference.rfind("builtin://", 0) == 0)
				{
					if (!AssetManager::GetMesh(reference))
						m_Report.Add(ValidationSeverity::Error, file, fmt::format("{} the unknown built-in mesh '{}'", role, reference));
					return;
				}
				const std::filesystem::path path = std::filesystem::path(StripSubAsset(reference)).lexically_normal();
				const auto first = path.begin();
				if (path.is_absolute() || path.has_root_name() || first == path.end() || *first != Project::AssetDirectoryName)
				{
					m_Report.Add(ValidationSeverity::Error, file, fmt::format("{} '{}', which is outside Assets/ and will not be exported", role, reference));
					return;
				}
				std::string actual;
				switch (FindExactFile(path, actual))
				{
					case Match::Exact:
						break;
					case Match::DifferentCase:
						m_Report.Add(ValidationSeverity::Error, file, fmt::format("{} '{}', but the file on disk is '{}' (letter case matters on Linux)", role, reference, actual));
						break;
					case Match::Missing:
						m_Report.Add(ValidationSeverity::Error, file, fmt::format("{} the missing file '{}'", role, reference));
						break;
				}
			}

			enum class Match
			{
				Exact,
				DifferentCase,
				Missing
			};

			// Walks the path one component at a time, comparing names exactly, so a case-insensitive file system
			// (Windows, macOS) cannot hide a misspelling that breaks the game on Linux.
			Match FindExactFile(const std::filesystem::path& relative, std::string& outActual)
			{
				std::filesystem::path current = m_Directory;
				std::filesystem::path actual;
				bool caseDiffers = false;
				for (const auto& component : relative)
				{
					const std::vector<std::string>& names = ListDirectory(current);
					const std::string name = component.string();
					if (std::find(names.begin(), names.end(), name) != names.end())
					{
						actual /= name;
						current /= name;
						continue;
					}
					const auto folded = std::find_if(names.begin(), names.end(), [&](const std::string& entry) { return Lowercase(entry) == Lowercase(name); });
					if (folded == names.end())
						return Match::Missing;
					caseDiffers = true;
					actual /= *folded;
					current /= *folded;
				}
				std::error_code error;
				if (!std::filesystem::is_regular_file(current, error))
					return Match::Missing;
				outActual = actual.generic_string();
				return caseDiffers ? Match::DifferentCase : Match::Exact;
			}

			const std::vector<std::string>& ListDirectory(const std::filesystem::path& directory)
			{
				auto [it, inserted] = m_Listings.try_emplace(directory.generic_string());
				if (inserted)
				{
					std::error_code error;
					for (auto entry = std::filesystem::directory_iterator(directory, error); !error && entry != std::filesystem::directory_iterator(); entry.increment(error))
						it->second.push_back(entry->path().filename().string());
				}
				return it->second;
			}

			void CheckScriptSyntax(const std::filesystem::path& path)
			{
				std::string error;
				if (!ScriptEngine::CheckSyntax(path.string(), error))
					m_Report.Add(ValidationSeverity::Error, Relative(path), "does not compile: " + error);
				else
					m_CompiledScripts.insert(Relative(path));
			}

			// A script a Script component uses must also return its class table when run.
			void CheckScriptClass(const std::string& script, const std::string& file)
			{
				if (!m_CompiledScripts.contains(script))
					return; // missing (reported as a reference) or a syntax error (reported once already)
				std::string error;
				if (!ScriptEngine::LoadScriptProperties((m_Directory / script).string(), error))
					m_Report.Add(ValidationSeverity::Error, script, "fails to load as a script class (used by " + file + "): " + error);
			}

			const Project& m_Project;
			std::filesystem::path m_Directory;
			ValidationReport m_Report;
			std::map<std::string, std::vector<std::string>> m_Listings;
			// Script path -> the first scene or prefab using it.
			std::map<std::string, std::string> m_ReferencedScripts;
			std::set<std::string> m_CompiledScripts;
		};

	}

	void ValidationReport::Add(ValidationSeverity severity, std::string file, std::string message)
	{
		(severity == ValidationSeverity::Error ? m_ErrorCount : m_WarningCount)++;
		if (m_Issues.size() < MaxIssues)
			m_Issues.push_back({ severity, std::move(file), std::move(message) });
	}

	ValidationReport ValidateProject(const Project& project)
	{
		return Validator(project).Run();
	}

	std::vector<UnknownPhysicsLayerUse> FindUnknownPhysicsLayers(Scene& scene, const PhysicsLayers& layers)
	{
		std::vector<UnknownPhysicsLayerUse> result;
		auto check = [&](Entity entity, const std::string& layer) {
			if (!layers.Find(layer))
				result.push_back({ entity.GetUUID(), entity.GetName(), layer });
		};
		for (entt::entity handle : scene.GetAllEntitiesWith<RigidBodyComponent>())
		{
			if (!scene.GetRegistry().all_of<CharacterControllerComponent>(handle))
				check(Entity(handle, &scene), scene.GetRegistry().get<RigidBodyComponent>(handle).Layer);
		}
		for (entt::entity handle : scene.GetAllEntitiesWith<CharacterControllerComponent>())
			check(Entity(handle, &scene), scene.GetRegistry().get<CharacterControllerComponent>(handle).Layer);
		return result;
	}

}
