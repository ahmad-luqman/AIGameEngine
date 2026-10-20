#include "Basalt/Animation/AnimationGraph.h"

#include "Basalt/Asset/AssetKey.h"
#include "Basalt/Asset/MaterialAsset.h"
#include "Basalt/Asset/MeshSource.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Project/ProjectValidator.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Components.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scene/SceneSerializer.h"
#include "Basalt/Scripting/ScriptEngine.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <system_error>

namespace Basalt {

	namespace {

		// File types components reference by path. A string ending in one of these is treated as an asset path.
		const std::set<std::string> s_AssetExtensions = { ".gltf", ".glb", ".ktx2", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".wav", ".mp3", ".ogg", ".flac", ".ttf", ".otf", ".lua", ".bscene", ".bprefab", ".banimgraph", ".bmaterial" };

		// "Assets/Models/Ship.glb#image/0" names an image inside Ship.glb (and "#material/0" one of its materials).
		std::string StripSubAsset(const std::string& reference)
		{
			return std::string(GetAssetSource(reference));
		}

		bool HasAssetExtension(const std::string& value)
		{
			return s_AssetExtensions.contains(FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(StripSubAsset(value))));
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
			{
			}

			ValidationReport Run()
			{
				AssetRegistry assets(m_Directory);
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
			enum class Match
			{
				Exact,
				DifferentCase,
				Missing,
				Unreadable
			};

			struct Listing
			{
				std::vector<std::string> Names;
				std::string Error;
			};

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
							for (const auto& [field, value] : fields.items())
							{
								if (value.is_string() && IsPathField(component, field))
								{
									if (!value.get_ref<const std::string&>().empty())
										paths.insert(value.get<std::string>());
								}
								else
									CollectAssetLikeStrings(value, assetLike);
							}
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
					m_Report.Add(ValidationSeverity::Error, file, "unresolved asset UUID reference: " + reference.substr(0, reference.find('/', AssetRegistry::UnresolvedPrefix.size())));
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
				const std::filesystem::path path = FileSystem::PathFromUtf8(StripSubAsset(reference)).lexically_normal();
				const auto first = path.begin();
				if (path.is_absolute() || path.has_root_name() || first == path.end() || *first != Project::AssetDirectoryName)
				{
					m_Report.Add(severity, file, fmt::format("{} '{}', which is outside Assets/ and will not be exported", role, reference));
					return;
				}
				std::string actual, problem;
				switch (FindExactFile(path, actual, problem))
				{
					case Match::Exact:
						break;
					case Match::DifferentCase:
						m_Report.Add(severity, file, fmt::format("{} '{}', but the file on disk is '{}' (letter case matters on Linux)", role, reference, actual));
						break;
					case Match::Missing:
						m_Report.Add(severity, file, fmt::format("{} the missing file '{}'", role, reference));
						break;
					case Match::Unreadable:
						m_Report.Add(severity, file, fmt::format("{} '{}', which cannot be checked: {}", role, reference, problem));
						break;
				}
			}

			// Walks the path one component at a time, comparing UTF-8 names exactly, so a case-insensitive file system
			// (Windows, macOS) cannot hide a misspelling that breaks the game on Linux.
			Match FindExactFile(const std::filesystem::path& relative, std::string& outActual, std::string& outProblem)
			{
				std::filesystem::path current = m_Directory;
				std::filesystem::path actual;
				bool caseDiffers = false;
				for (const auto& component : relative)
				{
					const Listing& listing = ListDirectory(current);
					if (!listing.Error.empty())
					{
						outProblem = "cannot list '" + FileSystem::PathToUtf8(current) + "': " + listing.Error;
						return Match::Unreadable;
					}
					const std::string name = FileSystem::PathToUtf8(component);
					auto found = std::find(listing.Names.begin(), listing.Names.end(), name);
					if (found == listing.Names.end())
					{
						const std::string folded = FileSystem::ToLowerAscii(name);
						found = std::find_if(listing.Names.begin(), listing.Names.end(), [&](const std::string& entry) { return FileSystem::ToLowerAscii(entry) == folded; });
						if (found == listing.Names.end())
							return Match::Missing;
						caseDiffers = true;
					}
					actual /= FileSystem::PathFromUtf8(*found);
					current /= FileSystem::PathFromUtf8(*found);
				}
				std::error_code error;
				const bool regular = std::filesystem::is_regular_file(current, error);
				if (error || !regular)
				{
					outProblem = error ? error.message() : "it is not a file";
					return Match::Unreadable;
				}
				outActual = FileSystem::PathToUtf8(actual.generic_u8string());
				return caseDiffers ? Match::DifferentCase : Match::Exact;
			}

			const Listing& ListDirectory(const std::filesystem::path& directory)
			{
				auto [it, inserted] = m_Listings.try_emplace(FileSystem::PathToUtf8(directory));
				if (inserted)
				{
					std::error_code error;
					for (auto entry = std::filesystem::directory_iterator(directory, error); !error && entry != std::filesystem::directory_iterator(); entry.increment(error))
						it->second.Names.push_back(FileSystem::PathToUtf8(entry->path().filename()));
					// A missing directory means a missing file, not an unreadable one.
					if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory)
						it->second.Error = error.message();
				}
				return it->second;
			}

			void CheckAnimationGraph(const std::filesystem::path& path)
			{
				std::string error;
				const auto text = FileSystem::ReadTextFile(path);
				const auto graph = text ? ParseJson(*text, error) : nlohmann::json();
				if (!text || graph.is_discarded() || !AnimationGraph::FromJson(graph, error))
					m_Report.Add(ValidationSeverity::Error, Relative(path), "invalid animation graph: " + (text ? error : "cannot read file"));
			}

			// A material must parse, and every texture it names must exist like a scene's references (unresolved UUIDs
			// included: the parser keeps them as opaque keys).
			void CheckMaterial(const std::filesystem::path& path)
			{
				std::string error;
				const auto text = FileSystem::ReadTextFile(path);
				const auto data = text ? ParseJson(*text, error) : nlohmann::json();
				const auto material = text && !data.is_discarded() ? MaterialAsset::FromJson(data, error) : std::nullopt;
				if (!material)
				{
					m_Report.Add(ValidationSeverity::Error, Relative(path), "invalid material: " + (text ? error : "cannot read file"));
					return;
				}
				for (size_t slot = 0; slot < MaterialTextureSlotCount; slot++)
					if (const std::string& map = MaterialAsset::GetMap(*material, static_cast<MaterialTextureSlot>(slot)); !map.empty())
						CheckReference(map, Relative(path), (std::string(MaterialAsset::MapFields[slot]) + " references").c_str());
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
			std::map<std::string, Listing> m_Listings;
			// Normalized script path -> the first scene or prefab using it.
			std::map<std::string, std::string> m_ReferencedScripts;
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
