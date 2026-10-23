#include "Basalt/Animation/AnimationGraph.h"

#include "Basalt/Asset/AssetKey.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Asset/MaterialAsset.h"
#include "Basalt/Asset/MeshImporter.h"
#include "Basalt/Asset/MeshSource.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Project/Packaging.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Project/ProjectValidator.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Components.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Prefab.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scene/SceneSerializer.h"
#include "Basalt/Scripting/ScriptEngine.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <system_error>

namespace Basalt {

	namespace {

		// File types components reference by path. A string ending in one of these is treated as an asset path.
		const std::set<std::string> s_AssetExtensions = { ".gltf", ".glb", ".ktx2", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".wav", ".mp3", ".ogg", ".flac", ".ttf", ".otf", ".lua", ".bscene", ".bprefab", ".banimgraph", ".bmaterial" };

		bool HasAssetExtension(const std::string& value)
		{
			return s_AssetExtensions.contains(FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(std::string(GetAssetSource(value)))));
		}

		// Component fields that hold an asset path the game loads: a broken one is an error. A MeshCollider's empty
		// Mesh means the entity's own mesh; an Animator's Clip names an animation, an AudioSource's is a file.
		bool IsPathField(const std::string& component, const std::string& field)
		{
			const auto* info = ComponentRegistry::Find(component);
			return info && std::find(info->AssetFields.begin(), info->AssetFields.end(), field) != info->AssetFields.end();
		}

		// Other strings (script properties, text) that name a file under Assets/: likely a path the game uses, but
		// possibly just text, so a broken one only warns.
		void CollectAssetLikeStrings(const nlohmann::json& value, std::set<std::string>& outStrings)
		{
			if (value.is_string())
			{
				if (const std::string& text = value.get_ref<const std::string&>(); text.rfind("Assets/", 0) == 0 && HasAssetExtension(text))
					outStrings.insert(text);
			}
			else if (value.is_structured())
			{
				for (const auto& element : value)
					CollectAssetLikeStrings(element, outStrings);
			}
		}

		// Walks the project's files and reports into a ValidationReport (see ValidateProject).
		class Validator
		{
		public:
			explicit Validator(const Project& project)
				: m_Project(project)
				, m_Directory(project.GetDirectory())
				, m_Assets(project.GetDirectory())
			{
			}

			ValidationReport Run()
			{
				AssetRegistry& assets = m_Assets;
				std::string metadataError;
				if (!assets.Refresh(metadataError))
					m_Report.Add(ValidationSeverity::Error, "Assets", metadataError);
				for (const auto& [path, problem] : assets.GetProblems())
					m_Report.Add(ValidationSeverity::Error, path + ".meta", problem);
				for (const auto& path : assets.GetOrphans())
					m_Report.Add(ValidationSeverity::Warning, path + ".meta", "orphan metadata: restoring the source at this path reuses its identity; delete both files when removing an asset");
				CheckProjectFile();
				CheckInputFile();
				CheckStartScene();
				CheckIcon();
				std::vector<std::filesystem::path> scenes, prefabs, scripts, graphs, materials;
				ListAssets(scenes, prefabs, scripts, graphs, materials);
				for (const auto& path : scenes)
					CheckScene(path);
				for (const auto& path : prefabs)
					CheckPrefab(path);
				for (const auto& path : scripts)
					CheckScriptSyntax(path);
				for (const auto& path : graphs)
					CheckAnimationGraph(path);
				for (const auto& path : materials)
					CheckMaterial(path);
				for (const auto& [script, file] : m_ReferencedScripts)
					CheckScriptClass(script, file);
				return std::move(m_Report);
			}

		private:
			std::string Relative(const std::filesystem::path& path) const
			{
				return FileSystem::PathToUtf8(path.lexically_relative(m_Directory).generic_u8string());
			}

			static std::string Normalized(const std::string& reference)
			{
				return FileSystem::PathToUtf8(FileSystem::PathFromUtf8(reference).lexically_normal().generic_u8string());
			}

			void CheckProjectFile()
			{
				const ProjectConfig& config = m_Project.GetConfig();
				if (config.WindowWidth < ProjectConfig::MinWindowSize || config.WindowWidth > ProjectConfig::MaxWindowSize || config.WindowHeight < ProjectConfig::MinWindowSize || config.WindowHeight > ProjectConfig::MaxWindowSize)
					m_Report.Add(ValidationSeverity::Error, Project::FileName, fmt::format("the window size {}x{} is outside [{}, {}]", config.WindowWidth, config.WindowHeight, ProjectConfig::MinWindowSize, ProjectConfig::MaxWindowSize));
				const auto text = FileSystem::ReadTextFile(m_Project.GetProjectFilePath());
				if (!text)
				{
					m_Report.Add(ValidationSeverity::Error, Project::FileName, "the project file cannot be read");
					return;
				}
				const nlohmann::json data = ParseJson(*text);
				if (!data.is_object())
				{
					m_Report.Add(ValidationSeverity::Error, Project::FileName, "the project file is not a JSON object");
					return;
				}
				const auto format = data.find("Format");
				if (format == data.end() || *format != "BasaltProject")
					m_Report.Add(ValidationSeverity::Warning, Project::FileName, "the file does not declare \"Format\": \"BasaltProject\"");
			}

			// A broken input actions file stops the project from loading anywhere (Project::Load), so this reports one
			// broken since the project was opened. Actions without bindings, and bindings that can never press a
			// Button action (Scale 0, or below 0 on a key, button or trigger) or move an Axis (Scale 0), are legal (a game
			// may rebind them at run time) but usually oversights.
			void CheckInputFile()
			{
				std::string error;
				const auto actions = Project::ReadInputActions(m_Project.GetInputFilePath(), error);
				if (!actions)
				{
					m_Report.Add(ValidationSeverity::Error, Project::InputFileName, error);
					return;
				}
				for (const InputAction& action : actions->GetActions())
				{
					if (action.Bindings.empty())
						m_Report.Add(ValidationSeverity::Warning, Project::InputFileName, "input action '" + action.Name + "' has no bindings");
					const bool isButton = action.Type == InputActionType::Button;
					for (size_t i = 0; i < action.Bindings.size(); i++)
					{
						const InputBinding& binding = action.Bindings[i];
						// A stick read with a negative Scale still presses a button when pushed the other way; keys, buttons
						// and triggers (0..1) never do.
						const auto* axis = std::get_if<GamepadAxis>(&binding.Control);
						const bool silent = binding.Scale == 0.0f || (isButton && binding.Scale < 0.0f && !(axis && !IsGamepadTrigger(*axis)));
						if (silent)
							m_Report.Add(ValidationSeverity::Warning, Project::InputFileName, fmt::format("input action '{}': binding {} can never {} (Scale {})", action.Name, i + 1, isButton ? "press it" : "move it", binding.Scale));
					}
				}
			}

			void CheckStartScene()
			{
				const std::string& start = m_Project.GetConfig().StartScene;
				if (!m_Project.GetStartSceneError().empty())
					m_Report.Add(ValidationSeverity::Error, Project::FileName, m_Project.GetStartSceneError());
				else if (start.empty())
					m_Report.Add(ValidationSeverity::Error, Project::FileName, "no StartScene is set (project.set startScene)");
				else if (FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(start)) != ".bscene")
					m_Report.Add(ValidationSeverity::Error, Project::FileName, "the StartScene '" + start + "' is not a .bscene file");
				else
					CheckReference(start, Project::FileName, "the StartScene names");
			}

			// The icon is optional; one that is set must be a PNG under Assets/ that packaging can decode.
			void CheckIcon()
			{
				const std::string& icon = m_Project.GetConfig().Icon;
				if (icon.empty())
					return;
				const size_t before = m_Report.GetErrorCount() + m_Report.GetWarningCount();
				CheckReference(icon, Project::FileName, "the Icon names");
				if (m_Report.GetErrorCount() + m_Report.GetWarningCount() != before)
					return;
				std::string error;
				const auto image = LoadIconImage(m_Project.GetIconPath(), error);
				if (!image)
					m_Report.Add(ValidationSeverity::Error, Project::FileName, "the Icon: " + error);
				else if (std::max(image->Width, image->Height) < RecommendedIconSize)
					m_Report.Add(ValidationSeverity::Warning, Project::FileName, fmt::format("the Icon '{}' is {} x {} pixels; packaged icons look blurry below {} pixels (512 or more is best)", icon, image->Width, image->Height, RecommendedIconSize));
			}

			void ListAssets(std::vector<std::filesystem::path>& scenes, std::vector<std::filesystem::path>& prefabs, std::vector<std::filesystem::path>& scripts, std::vector<std::filesystem::path>& graphs, std::vector<std::filesystem::path>& materials)
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
					std::error_code entryError;
					const bool regular = it->is_regular_file(entryError);
					if (entryError)
					{
						// A broken link or an unreadable entry: say so rather than leave it out of the checks.
						m_Report.Add(ValidationSeverity::Warning, Relative(it->path()), "cannot be read: " + entryError.message());
						continue;
					}
					if (!regular)
						continue;
					const std::string extension = FileSystem::GetLowercaseExtension(it->path());
					if (extension == ".bscene")
						scenes.push_back(it->path());
					else if (extension == ".bprefab")
						prefabs.push_back(it->path());
					else if (extension == ".banimgraph")
						graphs.push_back(it->path());
					else if (extension == MaterialAsset::Extension)
						materials.push_back(it->path());
					else if (extension == ".lua")
						scripts.push_back(it->path());
				}
				if (error)
					m_Report.Add(ValidationSeverity::Error, Project::AssetDirectoryName, "cannot list all the assets (the walk stopped with: " + error.message() + "); the remaining files were not checked");
				// Directory order differs between platforms; reports should not.
				for (auto* list : { &scenes, &prefabs, &scripts, &graphs, &materials })
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
				CheckPrefabInstances(*scene, Relative(path));
			}

			// A scene stores its instances' data, so instances of a prefab saved since the scene was stay as they were
			// until the scene is opened (which merges them) and saved.
			void CheckPrefabInstances(Scene& scene, const std::string& file)
			{
				std::map<std::string, size_t> outdated;
				for (auto [handle, link] : scene.GetAllEntitiesWith<PrefabComponent>().each())
				{
					if (link.Prefab.empty())
						continue;
					const std::string path = Prefab::NormalizePath(link.Prefab);
					// A prefab that does not load is reported by its own check; its instances are not outdated then.
					auto [source, inserted] = m_PrefabSources.try_emplace(path);
					if (nlohmann::json entities; inserted)
					{
						if (std::string error; Prefab::ReadSource(Project::ResolvePath(path), entities, error))
							source->second = std::move(entities);
					}
					if (const std::optional<nlohmann::json>& sourceEntities = source->second; sourceEntities && Prefab::IsOutdated(link, *sourceEntities))
						++outdated[path];
				}
				for (const auto& [prefab, count] : outdated)
					m_Report.Add(ValidationSeverity::Warning, file, fmt::format("{} instance(s) of '{}' predate its last save; open the scene (scene.open merges them, keeping overrides) and save it", count, prefab));
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
				std::set<std::string> paths, assetLike;
				if (const auto entities = data.find("Entities"); entities != data.end())
					for (const auto& entity : *entities)
					{
						const auto components = entity.find("Components");
						if (components == entity.end() || !components->is_object())
							continue;
						for (const auto& [component, fields] : components->items())
						{
							if (!fields.is_object())
								continue;
							if (const auto* info = ComponentRegistry::Find(component))
								for (const auto& assetField : info->AssetFields)
									ForEachAssetValue(fields, assetField, [&](const nlohmann::json& value, const std::string&) {
										if (value.is_string() && !value.get_ref<const std::string&>().empty())
											paths.insert(value.get<std::string>());
									});
							// Strings outside the asset fields that look like paths only warn (paths found above are skipped).
							for (const auto& [field, value] : fields.items())
								if (!(value.is_string() && IsPathField(component, field)))
									CollectAssetLikeStrings(value, assetLike);
						}
					}
				for (const std::string& path : paths)
					CheckReference(path, file, "references", ValidationSeverity::Error);
				for (const std::string& path : assetLike)
					if (!paths.contains(path))
						CheckReference(path, file, "has a value naming", ValidationSeverity::Warning);
				for (auto handle : scene.GetAllEntitiesWith<ScriptComponent>())
				{
					const std::string& script = scene.GetRegistry().get<ScriptComponent>(handle).Script;
					if (!script.empty())
						m_ReferencedScripts.emplace(Normalized(script), file);
				}
				for (const UnknownPhysicsLayerUse& use : FindUnknownPhysicsLayers(scene, m_Project.GetConfig().Physics))
					m_Report.Add(ValidationSeverity::Warning, file, fmt::format("'{}' uses the undefined physics layer '{}' (it plays on Default)", use.EntityName, use.Layer));
			}

			// A path a scene, prefab or the project refers to: it must name an existing file inside Assets/, spelled
			// with the file's exact letter case (the game may run on a case-sensitive file system).
			void CheckReference(const std::string& reference, const std::string& file, const char* role, ValidationSeverity severity = ValidationSeverity::Error)
			{
				if (AssetRegistry::IsUnresolvedKey(reference))
				{
					m_Report.Add(ValidationSeverity::Error, file, fmt::format("{} an {}", role, m_Assets.DescribeUnresolved(reference)));
					return;
				}

				if (Primitives::IsBuiltinKey(reference))
				{
					// Created, not looked up: AssetManager would log the failure and cache it, and validation changes nothing.
					if (!Primitives::CreateFromKey(reference))
						m_Report.Add(severity, file, fmt::format("{} the unknown built-in mesh '{}'", role, reference));
					return;
				}
				// A backslash separates folders on Windows only: the same game would not find the file on Linux or macOS.
				if (reference.find('\\') != std::string::npos)
				{
					m_Report.Add(severity, file, fmt::format("{} '{}', which uses backslashes; separate folders with / so it loads on every platform", role, reference));
					return;
				}
				const std::filesystem::path path = FileSystem::PathFromUtf8(std::string(GetAssetSource(reference))).lexically_normal();
				const auto first = path.begin();
				if (path.is_absolute() || path.has_root_name() || first == path.end() || *first != Project::AssetDirectoryName)
				{
					m_Report.Add(severity, file, fmt::format("{} '{}', which is outside Assets/ and will not be exported", role, reference));
					return;
				}
				std::string actual, problem;
				switch (FileSystem::FindExactCase(m_Directory, path, actual, problem, &m_Listings))
				{
					case FileSystem::PathMatch::Exact:
						CheckSubAsset(reference, file, role, severity);
						break;
					case FileSystem::PathMatch::DifferentCase:
						m_Report.Add(severity, file, fmt::format("{} '{}', but the file on disk is '{}' (letter case matters on Linux)", role, reference, actual));
						break;
					case FileSystem::PathMatch::Missing:
						m_Report.Add(severity, file, fmt::format("{} the missing file '{}'", role, reference));
						break;
					case FileSystem::PathMatch::Unreadable:
						m_Report.Add(severity, file, fmt::format("{} '{}', which cannot be checked: {}", role, reference, problem));
						break;
				}
			}

			// A "#image/<n>" or "#material/<n>" reference names a model's embedded image or imported material: the model must
			// be a readable glTF that has it (an embedded image, not an external file). The JSON is read once per model;
			// the buffers are not loaded.
			void CheckSubAsset(const std::string& reference, const std::string& file, const char* role, ValidationSeverity severity)
			{
				if (GetSubAssetFragment(reference).empty())
					return;
				const std::string model(GetAssetSource(reference));
				if (!AssetManager::IsMeshFile(model))
				{
					m_Report.Add(severity, file, fmt::format("{} '{}', which is not a model's embedded image or material (<model>#image/<n>, <model>#material/<n>)", role, reference));
					return;
				}
				auto [entry, inserted] = m_ModelContents.try_emplace(model);
				if (inserted)
					entry->second.Contents = MeshImporter::Inspect(m_Directory / FileSystem::PathFromUtf8(model), entry->second.Error);
				const std::optional<MeshImporter::Contents>& contents = entry->second.Contents;
				if (!contents)
					m_Report.Add(severity, file, fmt::format("{} '{}', but '{}' is {}", role, reference, model, entry->second.Error));
				else if (const auto problem = MeshImporter::CheckSubAsset(*contents, reference))
					m_Report.Add(severity, file, fmt::format("{} '{}', but {}", role, reference, *problem));
			}

			void CheckAnimationGraph(const std::filesystem::path& path)
			{
				std::string error;
				const auto text = FileSystem::ReadTextFile(path);
				const auto graph = text ? ParseJson(*text, error) : nlohmann::json();
				if (!text || graph.is_discarded() || !AnimationGraph::FromJson(graph, error))
					m_Report.Add(ValidationSeverity::Error, Relative(path), "invalid animation graph: " + (text ? error : "cannot read file"));
			}

			// A material must parse (its UUIDs through this project's registry), and every texture it names must be a texture
			// that exists like a scene's references (unresolved UUIDs included: the parser keeps them as opaque keys).
			void CheckMaterial(const std::filesystem::path& path)
			{
				std::string error;
				const auto text = FileSystem::ReadTextFile(path);
				const auto material = text ? MaterialAsset::FromText(*text, error, nullptr, &m_Assets) : std::nullopt;
				if (!material)
				{
					m_Report.Add(ValidationSeverity::Error, Relative(path), "invalid material: " + (text ? error : "cannot read file"));
					return;
				}
				for (const MaterialAsset::SlotInfo& slot : MaterialAsset::Slots)
				{
					const std::string& map = *material.*slot.Map;
					if (map.empty())
						continue;
					const std::string role = std::string(slot.MapField) + " references";
					if (!AssetRegistry::IsUnresolvedKey(map) && !MaterialAsset::IsTextureKey(map))
						m_Report.Add(ValidationSeverity::Error, Relative(path), fmt::format("{} '{}', which is not a texture (an image file or <model>#image/<n>)", role, map));
					else
						CheckReference(map, Relative(path), role.c_str());
				}
			}

			void CheckScriptSyntax(const std::filesystem::path& path)
			{
				std::string error;
				if (!ScriptEngine::CheckSyntax(FileSystem::PathToUtf8(path), error))
					m_Report.Add(ValidationSeverity::Error, Relative(path), "does not compile: " + error);
				else
					m_CompiledScripts.insert(Relative(path));
			}

			// A script a Script component uses must also return its class table when its top level runs.
			void CheckScriptClass(const std::string& script, const std::string& file)
			{
				if (!m_CompiledScripts.contains(script))
					return; // missing (reported as a reference) or a syntax error (reported once already)
				std::string error;
				if (!ScriptEngine::LoadScriptProperties(FileSystem::PathToUtf8(m_Directory / FileSystem::PathFromUtf8(script)), error))
					m_Report.Add(ValidationSeverity::Error, script, "fails to load as a script class (used by " + file + "): " + error);
			}

			const Project& m_Project;
			std::filesystem::path m_Directory;
			ValidationReport m_Report;
			FileSystem::DirectoryListings m_Listings;
			// Decodes material files' UUIDs: this project's, whichever project is active.
			AssetRegistry m_Assets;
			// What each model referenced through a sub-asset key contains (or why it could not be read).
			struct ModelContents
			{
				std::optional<MeshImporter::Contents> Contents;
				std::string Error;
			};
			std::map<std::string, ModelContents> m_ModelContents;
			// Normalized script path -> the first scene or prefab using it.
			std::map<std::string, std::string> m_ReferencedScripts;
			// Each prefab's validated source, read once per validation (nothing when it does not load).
			std::map<std::string, std::optional<nlohmann::json>> m_PrefabSources;
			std::set<std::string> m_CompiledScripts;
		};

	}

	void ValidationReport::Add(ValidationSeverity severity, std::string file, std::string message)
	{
		const bool error = severity == ValidationSeverity::Error;
		(error ? m_ErrorCount : m_WarningCount)++;
		if (m_Issues.size() >= MaxIssues)
		{
			// Full: an error displaces the last listed warning, so warnings can never hide every error.
			if (!error)
				return;
			const auto warning = std::find_if(m_Issues.rbegin(), m_Issues.rend(), [](const ValidationIssue& issue) { return issue.Severity == ValidationSeverity::Warning; });
			if (warning == m_Issues.rend())
				return;
			m_Issues.erase(std::next(warning).base());
		}
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
