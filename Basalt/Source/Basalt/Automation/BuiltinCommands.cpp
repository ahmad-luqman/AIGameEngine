#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Asset/ImageUtils.h"
#include "Basalt/Asset/TextureSource.h"
#include "Basalt/Automation/AutomationSession.h"
#include "Basalt/Automation/CommandRegistry.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Input.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Project/Exporter.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scene/SceneSerializer.h"
#include "Basalt/Scripting/ScriptEngine.h"

#include <algorithm>
#include <optional>
#include <system_error>

namespace Basalt {

	namespace {

		using json = nlohmann::json;

		// --- Parameter helpers ----------------------------------------------------------------------

		const json& Require(const json& params, const char* name)
		{
			auto it = params.find(name);
			if (it == params.end())
				throw CommandError(std::string("missing parameter '") + name + "'");
			return *it;
		}

		std::string RequireString(const json& params, const char* name)
		{
			const json& value = Require(params, name);
			if (!value.is_string() || value.get<std::string>().empty())
				throw CommandError(std::string("parameter '") + name + "' must be a non-empty string");
			return value.get<std::string>();
		}

		std::string OptionalString(const json& params, const char* name, const std::string& fallback = "")
		{
			auto it = params.find(name);
			if (it == params.end() || it->is_null())
				return fallback;
			if (!it->is_string())
				throw CommandError(std::string("parameter '") + name + "' must be a string");
			return it->get<std::string>();
		}

		int64_t OptionalInteger(const json& params, const char* name, int64_t fallback, int64_t min, int64_t max)
		{
			auto it = params.find(name);
			if (it == params.end() || it->is_null())
				return fallback;
			if (!it->is_number_integer())
				throw CommandError(std::string("parameter '") + name + "' must be an integer");
			const int64_t value = it->get<int64_t>();
			if (value < min || value > max)
				throw CommandError(std::string("parameter '") + name + "' must be in [" + std::to_string(min) + ", " + std::to_string(max) + "]");
			return value;
		}

		double OptionalNumber(const json& params, const char* name, double fallback, double min, double max)
		{
			auto it = params.find(name);
			if (it == params.end() || it->is_null())
				return fallback;
			if (!it->is_number())
				throw CommandError(std::string("parameter '") + name + "' must be a number");
			const double value = it->get<double>();
			if (!(value >= min && value <= max))
				throw CommandError(std::string("parameter '") + name + "' must be in [" + std::to_string(min) + ", " + std::to_string(max) + "]");
			return value;
		}

		bool OptionalBool(const json& params, const char* name, bool fallback)
		{
			auto it = params.find(name);
			if (it == params.end() || it->is_null())
				return fallback;
			if (!it->is_boolean())
				throw CommandError(std::string("parameter '") + name + "' must be true or false");
			return it->get<bool>();
		}

		const PhysicsLayers& ActivePhysicsLayers()
		{
			static const PhysicsLayers s_DefaultLayers;
			const Ref<Project>& project = Project::GetActive();
			return project ? project->GetConfig().Physics : s_DefaultLayers;
		}

		// Entities whose RigidBody names a layer the layers do not define (they play on Default), as
		// [{entity, name, layer}] in registry order.
		json FindUnknownPhysicsLayers(Scene& scene, const PhysicsLayers& layers)
		{
			json result = json::array();
			for (entt::entity handle : scene.GetAllEntitiesWith<RigidBodyComponent>())
			{
				Entity entity(handle, &scene);
				const std::string& layer = entity.GetComponent<RigidBodyComponent>().Layer;
				if (!layers.Find(layer))
					result.push_back({ { "entity", static_cast<uint64_t>(entity.GetUUID()) }, { "name", entity.GetName() }, { "layer", layer } });
			}
			return result;
		}

		Scene& RequireScene(AutomationSession& session)
		{
			if (!session.GetScene())
				throw CommandError("no scene is open");
			return *session.GetScene();
		}

		Ref<Project> RequireProject()
		{
			const Ref<Project>& project = Project::GetActive();
			if (!project)
				throw CommandError("no project is open (use project.open or project.create)");
			return project;
		}

		// Entities are addressed by numeric ID or by (unique) name.
		Entity ResolveEntity(Scene& scene, const json& reference, const char* parameter = "entity")
		{
			if (reference.is_number_unsigned() || reference.is_number_integer())
			{
				Entity entity = scene.GetEntityByUUID(reference.get<uint64_t>());
				if (!entity)
					throw CommandError("no entity with ID " + std::to_string(reference.get<uint64_t>()));
				return entity;
			}
			if (reference.is_string())
			{
				const std::vector<Entity> matches = scene.FindEntitiesByName(reference.get<std::string>());
				if (matches.empty())
					throw CommandError("no entity named '" + reference.get<std::string>() + "'");
				if (matches.size() > 1)
					throw CommandError("several entities are named '" + reference.get<std::string>() + "'; use the numeric ID");
				return matches.front();
			}
			throw CommandError(std::string("parameter '") + parameter + "' must be an entity ID (number) or name (string)");
		}

		// Component data may name entities in reference fields (Joint "ConnectedEntity": "Door"); files and
		// the registry store UUIDs, so names are resolved here with the same rules as the "entity" parameter.
		// Numeric IDs must name an existing entity too (0 = none). Scene and prefab files stay lenient: they
		// may be loaded before the entities they refer to exist.
		json ResolveEntityFields(Scene& scene, const std::string& component, json data)
		{
			const ComponentInfo* info = ComponentRegistry::Find(component);
			if (!info || !data.is_object())
				return data;
			for (const std::string& field : info->EntityFields)
			{
				auto it = data.find(field);
				if (it == data.end())
					continue;
				const bool isNumber = it->is_number_unsigned() || it->is_number_integer();
				if (it->is_string() || (isNumber && it->get<uint64_t>() != 0))
					*it = static_cast<uint64_t>(ResolveEntity(scene, *it, field.c_str()).GetUUID());
			}
			return data;
		}

		Entity RequireEntity(AutomationSession& session, const json& params, const char* name = "entity")
		{
			return ResolveEntity(RequireScene(session), Require(params, name), name);
		}

		json EntitySummary(Entity entity)
		{
			json components = json::array();
			for (const ComponentInfo& info : ComponentRegistry::GetAll())
			{
				if (info.Has(entity))
					components.push_back(info.Name);
			}
			Entity parent = entity.GetParent();
			return {
				{ "id", static_cast<uint64_t>(entity.GetUUID()) },
				{ "name", entity.GetName() },
				{ "parent", parent ? static_cast<uint64_t>(parent.GetUUID()) : 0 },
				{ "components", components },
			};
		}

		// Edits made while playing affect only the play copy; warn callers so they do not lose work.
		void MarkEdited(AutomationSession& session)
		{
			if (!session.IsPlaying())
				session.MarkDirty();
		}

		std::string ProjectRelative(const std::string& path)
		{
			if (path.find("..") != std::string::npos)
				throw CommandError("paths must stay inside the project ('..' is not allowed)");
			if (std::filesystem::path(path).is_absolute())
				throw CommandError("asset paths must be project-relative (e.g. 'Assets/Scripts/Player.lua')");
			return path;
		}

		void Add(CommandRegistry& registry, std::string name, std::string description, std::map<std::string, std::string> parameters, std::function<json(AutomationSession&, const json&)> handler)
		{
			registry.Register({ std::move(name), std::move(description), std::move(parameters), std::move(handler) });
		}

		void ApplyKeyInput(const json& params)
		{
			const std::string name = RequireString(params, "key");
			const auto key = KeyCodeFromString(name);
			if (!key)
				throw CommandError("unknown key '" + name + "'");
			Input::SetKeyState(*key, OptionalBool(params, "down", true));
		}

		void ApplyMouseInput(const json& params)
		{
			if (auto position = params.find("position"); position != params.end())
			{
				glm::vec2 mouse;
				if (!position->is_array() || position->size() != 2 || !JsonToFloat((*position)[0], mouse.x) || !JsonToFloat((*position)[1], mouse.y))
					throw CommandError("'position' must be [x, y]");
				Input::SetMousePosition(mouse);
			}
			if (params.contains("button"))
			{
				const std::string name = RequireString(params, "button");
				const auto button = MouseCodeFromString(name);
				if (!button)
					throw CommandError("unknown mouse button '" + name + "'");
				Input::SetMouseButtonState(*button, OptionalBool(params, "down", true));
			}
		}

		// --- Command groups -------------------------------------------------------------------------

		void RegisterGeneral(CommandRegistry& registry)
		{
			Add(registry, "help", "Lists every command with its description and parameters.", {}, [&registry](AutomationSession&, const json&) {
				json commands = json::array();
				for (const auto& [name, info] : registry.GetCommands())
					commands.push_back({ { "command", name }, { "description", info.Description }, { "params", info.Parameters } });
				return json{ { "commands", commands } };
			});

			Add(registry, "component.types", "Lists every component type with its fields (for component.add/set).", {}, [](AutomationSession&, const json&) {
				json types = json::array();
				for (const ComponentInfo& info : ComponentRegistry::GetAll())
					types.push_back({ { "name", info.Name }, { "fields", info.Fields }, { "core", info.IsCore } });
				return json{ { "components", types } };
			});

			Add(registry, "log.get", "Returns engine log messages logged after index 'since'.", { { "since", "integer, cursor from a previous call (default 0)" }, { "errorsOnly", "bool" } }, [](AutomationSession&, const json& params) {
				const uint64_t since = static_cast<uint64_t>(OptionalInteger(params, "since", 0, 0, INT64_MAX));
				const bool errorsOnly = OptionalBool(params, "errorsOnly", false);
				uint64_t next = 0;
				json messages = json::array();
				static const char* s_Levels[] = { "trace", "info", "warn", "error", "critical" };
				for (const LogMessage& message : Log::GetHistory().GetMessagesSince(since, next))
				{
					if (errorsOnly && message.Level < LogLevel::Error)
						continue;
					messages.push_back({ { "level", s_Levels[static_cast<int>(message.Level)] }, { "logger", message.LoggerName }, { "text", message.Text } });
				}
				return json{ { "messages", messages }, { "next", next } };
			});
		}

		void RegisterProject(CommandRegistry& registry)
		{
			Add(registry, "project.create", "Creates a new project directory (Project.bproject + Assets/) and opens it.", { { "path", "string, project directory" }, { "name", "string" } }, [](AutomationSession& session, const json& params) {
				std::string error;
				Ref<Project> project = Project::Create(RequireString(params, "path"), OptionalString(params, "name", "Game"), error);
				if (!project)
					throw CommandError(error);
				Project::SetActive(project);
				AssetManager::Clear();
				session.SetEditScene(CreateRef<Scene>("Untitled"), "");
				return json{ { "directory", project->GetDirectory().string() } };
			});

			Add(registry, "project.open", "Opens a project (directory or .bproject file) and its start scene.", { { "path", "string" } }, [](AutomationSession& session, const json& params) {
				std::string error;
				Ref<Project> project = Project::Load(RequireString(params, "path"), error);
				if (!project)
					throw CommandError(error);
				Project::SetActive(project);
				AssetManager::Clear();
				const std::string startScene = project->GetConfig().StartScene;
				if (!startScene.empty())
				{
					Ref<Scene> scene = SceneSerializer::LoadScene(Project::ResolvePath(startScene), error);
					if (!scene)
						throw CommandError(error);
					session.SetEditScene(scene, startScene);
				}
				else
				{
					session.SetEditScene(CreateRef<Scene>("Untitled"), "");
				}
				return json{ { "name", project->GetConfig().Name }, { "scene", startScene } };
			});

			Add(registry, "project.info", "Returns the active project's settings.", {}, [](AutomationSession&, const json&) {
				Ref<Project> project = RequireProject();
				const ProjectConfig& config = project->GetConfig();
				return json{
					{ "name", config.Name },
					{ "directory", project->GetDirectory().string() },
					{ "startScene", config.StartScene },
					{ "windowWidth", config.WindowWidth },
					{ "windowHeight", config.WindowHeight },
					{ "fullscreen", config.Fullscreen },
					{ "physicsLayers", config.Physics.ToJson() },
				};
			});

			Add(registry, "project.set", "Changes project settings and saves the project file.", { { "name", "string" }, { "startScene", "string, project-relative scene" }, { "windowWidth", "integer" }, { "windowHeight", "integer" }, { "fullscreen", "bool" }, { "physicsLayers", "object {Names: [\"Default\", ...], IgnoredPairs: [[a, b], ...]}, replaces all layers; applies from the next play" } }, [](AutomationSession& session, const json& params) {
				Ref<Project> project = RequireProject();
				// Every value is read and validated before any is assigned, so a bad parameter leaves the
				// project untouched.
				ProjectConfig config = project->GetConfig();
				config.Name = OptionalString(params, "name", config.Name);
				config.StartScene = OptionalString(params, "startScene", config.StartScene);
				config.WindowWidth = static_cast<uint32_t>(OptionalInteger(params, "windowWidth", config.WindowWidth, 64, 16384));
				config.WindowHeight = static_cast<uint32_t>(OptionalInteger(params, "windowHeight", config.WindowHeight, 64, 16384));
				config.Fullscreen = OptionalBool(params, "fullscreen", config.Fullscreen);
				if (const auto it = params.find("physicsLayers"); it != params.end())
				{
					std::string error;
					auto layers = PhysicsLayers::FromJson(*it, error);
					if (!layers)
						throw CommandError("physicsLayers: " + error);
					config.Physics = std::move(*layers);
				}
				project->GetConfig() = std::move(config);
				std::string error;
				if (!project->Save(error))
					throw CommandError(error);
				// Renaming or removing a layer strands bodies on it; report them now rather than at play time.
				json result = json::object();
				if (session.GetScene())
					result["unknownPhysicsLayers"] = FindUnknownPhysicsLayers(*session.GetScene(), project->GetConfig().Physics);
				return result;
			});

			Add(registry, "project.export", "Exports a standalone, distributable build of the game (runtime + assets).", { { "output", "string, output directory" } }, [](AutomationSession& session, const json& params) {
				Ref<Project> project = RequireProject();
				if (session.IsDirty())
					throw CommandError("the open scene has unsaved changes; run scene.save first");
				ExportResult result = Exporter::Export(*project, RequireString(params, "output"));
				if (!result.Success)
					throw CommandError(result.Error);
				return json{ { "executable", result.ExecutablePath.string() }, { "files", result.FileCount } };
			});
		}

		void RegisterScene(CommandRegistry& registry)
		{
			Add(registry, "scene.new", "Replaces the open scene with an empty one (with a camera and sun unless empty=true).", { { "name", "string" }, { "empty", "bool" } }, [](AutomationSession& session, const json& params) {
				Ref<Scene> scene = CreateRef<Scene>(OptionalString(params, "name", "Untitled"));
				if (!OptionalBool(params, "empty", false))
				{
					Entity camera = scene->CreateEntity("Camera");
					camera.GetTransform().Translation = { 0.0f, 2.0f, 8.0f };
					camera.GetTransform().SetRotationEuler(glm::radians(glm::vec3(-10.0f, 0.0f, 0.0f)));
					camera.AddComponent<CameraComponent>();
					Entity sun = scene->CreateEntity("Sun");
					sun.GetTransform().SetRotationEuler(glm::radians(glm::vec3(-50.0f, 30.0f, 0.0f)));
					sun.AddComponent<DirectionalLightComponent>();
					scene->CreateEntity("Sky").AddComponent<SkyLightComponent>();
				}
				session.SetEditScene(scene, "");
				session.MarkDirty();
				return json{ { "entities", scene->GetEntityCount() } };
			});

			Add(registry, "scene.open", "Opens a scene file (project-relative path).", { { "path", "string" } }, [](AutomationSession& session, const json& params) {
				const std::string path = ProjectRelative(RequireString(params, "path"));
				std::string error;
				Ref<Scene> scene = SceneSerializer::LoadScene(Project::ResolvePath(path), error);
				if (!scene)
					throw CommandError(error);
				session.SetEditScene(scene, path);
				return json{ { "name", scene->GetName() }, { "entities", scene->GetEntityCount() } };
			});

			Add(registry, "scene.save", "Saves the edit scene (to 'path', or the path it was opened from).", { { "path", "string, project-relative, optional" } }, [](AutomationSession& session, const json& params) {
				std::string path = OptionalString(params, "path", session.GetScenePath());
				if (path.empty())
					throw CommandError("the scene has never been saved; pass 'path' (e.g. 'Assets/Scenes/Main.bscene')");
				path = ProjectRelative(path);
				std::string error;
				if (!SceneSerializer::SaveScene(*session.GetEditScene(), Project::ResolvePath(path), error))
					throw CommandError(error);
				session.SetScenePath(path);
				session.ClearDirty();
				return json{ { "path", path } };
			});

			Add(registry, "scene.hash", "Hash of the current scene state (the play copy while playing); equal states give equal hashes, whatever the entity UUIDs.", {}, [](AutomationSession& session, const json&) {
				return json{ { "hash", SceneSerializer::ComputeStateHash(RequireScene(session)) } };
			});

			Add(registry, "scene.get", "Returns the whole open scene as JSON (same format as .bscene files).", {}, [](AutomationSession& session, const json&) {
				return SceneSerializer::SerializeScene(RequireScene(session));
			});

			Add(registry, "scene.info", "Summary of the open scene: name, path, entity count, state, unsaved changes, rigid bodies on undefined physics layers.", {}, [](AutomationSession& session, const json&) {
				Scene& scene = RequireScene(session);
				return json{
					{ "name", scene.GetName() },
					{ "path", session.GetScenePath() },
					{ "entities", scene.GetEntityCount() },
					{ "playing", session.IsPlaying() },
					{ "dirty", session.IsDirty() },
					{ "unknownPhysicsLayers", FindUnknownPhysicsLayers(scene, ActivePhysicsLayers()) },
				};
			});

			Add(registry, "scene.settings", "Patches scene-level settings: {Renderer: {...}, Physics: {...}, Name}.", { { "Renderer", "object, e.g. {Exposure, Tonemapper, SSAOEnabled, ShadowDistance}" }, { "Physics", "object, e.g. {Gravity: [0,-9.81,0]}" }, { "Name", "string" } }, [](AutomationSession& session, const json& params) {
				Scene& scene = RequireScene(session);
				json patch = json::object();
				for (const char* key : { "Renderer", "Physics", "Name" })
				{
					if (params.contains(key))
						patch[key] = params[key];
				}
				for (auto it = params.begin(); it != params.end(); ++it)
				{
					if (!patch.contains(it.key()))
						throw CommandError("unknown setting group '" + it.key() + "' (valid: Renderer, Physics, Name)");
				}
				// Validate against a scratch scene first so a bad patch changes nothing.
				Scene scratch;
				scratch.GetRendererSettings() = scene.GetRendererSettings();
				scratch.GetPhysicsSettings() = scene.GetPhysicsSettings();
				scratch.SetName(scene.GetName());
				std::string error;
				if (!SceneSerializer::DeserializeScene(scratch, patch, error))
					throw CommandError(error);
				scene.GetRendererSettings() = scratch.GetRendererSettings();
				scene.GetPhysicsSettings() = scratch.GetPhysicsSettings();
				scene.SetName(scratch.GetName());
				MarkEdited(session);
				const json saved = SceneSerializer::SerializeScene(scene);
				return json{ { "Renderer", saved["Renderer"] }, { "Physics", saved["Physics"] }, { "Name", saved["Name"] } };
			});
		}

		void RegisterEntity(CommandRegistry& registry)
		{
			Add(registry, "entity.create", "Creates an entity, optionally with a parent and components ({\"Mesh\": {...}, ...}).", { { "name", "string" }, { "parent", "entity ID or name" }, { "components", "object: component name -> fields; entity references (Joint BodyEntity, ConnectedEntity) take an ID or name" } }, [](AutomationSession& session, const json& params) {
				Scene& scene = RequireScene(session);
				Entity parent = params.contains("parent") && !params["parent"].is_null() ? ResolveEntity(scene, params["parent"], "parent") : Entity{};
				// References are resolved before the entity exists, so a name never resolves to the entity
				// being created and a bad reference leaves nothing behind.
				json components;
				if (auto it = params.find("components"); it != params.end())
				{
					components = *it;
					if (components.is_object())
					{
						for (auto component = components.begin(); component != components.end(); ++component)
							component.value() = ResolveEntityFields(scene, component.key(), component.value());
					}
				}
				Entity entity = scene.CreateEntity(OptionalString(params, "name", "Entity"));
				if (parent)
					scene.SetParent(entity, parent, false);
				std::string error;
				if (!components.is_null() && !SceneSerializer::DeserializeEntityComponents(entity, { { "Components", components } }, error))
				{
					scene.DestroyEntity(entity);
					throw CommandError(error);
				}
				if (session.IsPlaying() && scene.GetScriptEngine())
					scene.GetScriptEngine()->EnsureInstance(entity);
				MarkEdited(session);
				return EntitySummary(entity);
			});

			Add(registry, "entity.destroy", "Destroys an entity and its children.", { { "entity", "ID or name" } }, [](AutomationSession& session, const json& params) {
				Entity entity = RequireEntity(session, params);
				const uint64_t id = entity.GetUUID();
				RequireScene(session).DestroyEntity(entity);
				MarkEdited(session);
				return json{ { "destroyed", id } };
			});

			Add(registry, "entity.get", "Returns an entity with all component data.", { { "entity", "ID or name" } }, [](AutomationSession& session, const json& params) {
				return SceneSerializer::SerializeEntity(RequireEntity(session, params));
			});

			Add(registry, "entity.list", "Lists entities (hierarchy order) with their components; optional name substring / component filter.", { { "nameContains", "string" }, { "component", "string" } }, [](AutomationSession& session, const json& params) {
				Scene& scene = RequireScene(session);
				const std::string contains = OptionalString(params, "nameContains");
				const std::string component = OptionalString(params, "component");
				const ComponentInfo* info = nullptr;
				if (!component.empty())
				{
					info = ComponentRegistry::Find(component);
					if (!info)
						throw CommandError("unknown component '" + component + "'");
				}
				json entities = json::array();
				for (Entity entity : scene.GetAllEntitiesOrdered())
				{
					if (!contains.empty() && entity.GetName().find(contains) == std::string::npos)
						continue;
					if (info && !info->Has(entity))
						continue;
					entities.push_back(EntitySummary(entity));
				}
				return json{ { "entities", entities } };
			});

			Add(registry, "entity.rename", "Renames an entity.", { { "entity", "ID or name" }, { "name", "string" } }, [](AutomationSession& session, const json& params) {
				Entity entity = RequireEntity(session, params);
				entity.GetComponent<TagComponent>().Tag = RequireString(params, "name");
				MarkEdited(session);
				return EntitySummary(entity);
			});

			Add(registry, "entity.set_parent", "Re-parents an entity (parent null = root), keeping its world transform unless keepWorld=false.", { { "entity", "ID or name" }, { "parent", "ID, name or null" }, { "keepWorld", "bool (default true)" } }, [](AutomationSession& session, const json& params) {
				Scene& scene = RequireScene(session);
				Entity entity = RequireEntity(session, params);
				Entity parent = params.contains("parent") && !params["parent"].is_null() ? ResolveEntity(scene, params["parent"], "parent") : Entity{};
				if (!scene.SetParent(entity, parent, OptionalBool(params, "keepWorld", true)))
					throw CommandError("cannot parent an entity to itself or to one of its descendants");
				MarkEdited(session);
				return EntitySummary(entity);
			});

			Add(registry, "entity.duplicate", "Duplicates an entity and its children (new IDs).", { { "entity", "ID or name" }, { "name", "string, optional" } }, [](AutomationSession& session, const json& params) {
				Scene& scene = RequireScene(session);
				Entity copy = scene.DuplicateEntity(RequireEntity(session, params));
				if (params.contains("name"))
					copy.GetComponent<TagComponent>().Tag = RequireString(params, "name");
				MarkEdited(session);
				return EntitySummary(copy);
			});

			Add(registry, "entity.transform", "Returns local and world transforms of an entity.", { { "entity", "ID or name" } }, [](AutomationSession& session, const json& params) {
				Scene& scene = RequireScene(session);
				Entity entity = RequireEntity(session, params);
				glm::vec3 position;
				glm::quat rotation;
				glm::vec3 scale;
				Math::DecomposeTransform(scene.GetWorldTransform(entity), position, rotation, scale);
				const glm::vec3 euler = glm::degrees(Math::EulerFromQuat(rotation));
				return json{
					{ "local", ComponentRegistry::Find("Transform")->Serialize(entity) },
					{ "world", { { "Translation", { position.x, position.y, position.z } }, { "Rotation", { euler.x, euler.y, euler.z } }, { "Scale", { scale.x, scale.y, scale.z } } } },
				};
			});
		}

		void RegisterComponents(CommandRegistry& registry)
		{
			Add(registry, "component.set", "Adds the component if missing and sets the given fields (others keep their values).", { { "entity", "ID or name" }, { "component", "string, e.g. 'RigidBody'" }, { "data", "object of fields; entity references (Joint BodyEntity, ConnectedEntity) take an ID or name" } }, [](AutomationSession& session, const json& params) {
				Entity entity = RequireEntity(session, params);
				const std::string name = RequireString(params, "component");
				std::string error;
				Scene& scene = RequireScene(session);
				if (!ComponentRegistry::AddOrPatch(entity, name, ResolveEntityFields(scene, name, params.value("data", json::object())), error))
					throw CommandError(error);
				if (name == "Script" && session.IsPlaying() && scene.GetScriptEngine())
					scene.GetScriptEngine()->EnsureInstance(entity);
				MarkEdited(session);
				return ComponentRegistry::Find(name)->Serialize(entity);
			});

			Add(registry, "component.get", "Returns one component's fields.", { { "entity", "ID or name" }, { "component", "string" } }, [](AutomationSession& session, const json& params) {
				Entity entity = RequireEntity(session, params);
				const std::string name = RequireString(params, "component");
				const ComponentInfo* info = ComponentRegistry::Find(name);
				if (!info)
					throw CommandError("unknown component '" + name + "' (valid: " + ComponentRegistry::GetNameList() + ")");
				if (!info->Has(entity))
					throw CommandError("entity '" + entity.GetName() + "' has no " + name + " component");
				return info->Serialize(entity);
			});

			Add(registry, "component.remove", "Removes a component.", { { "entity", "ID or name" }, { "component", "string" } }, [](AutomationSession& session, const json& params) {
				Entity entity = RequireEntity(session, params);
				const std::string name = RequireString(params, "component");
				const ComponentInfo* info = ComponentRegistry::Find(name);
				if (!info)
					throw CommandError("unknown component '" + name + "'");
				if (info->IsCore)
					throw CommandError("component '" + name + "' cannot be removed");
				if (!info->Has(entity))
					throw CommandError("entity '" + entity.GetName() + "' has no " + name + " component");
				info->Remove(entity);
				MarkEdited(session);
				return json::object();
			});
		}

		void RegisterAssets(CommandRegistry& registry)
		{
			Add(registry, "asset.list", "Lists project asset files, optionally filtered by extensions (e.g. ['.lua']).", { { "extensions", "array of strings" } }, [](AutomationSession&, const json& params) {
				RequireProject();
				std::vector<std::string> extensions;
				if (auto it = params.find("extensions"); it != params.end())
				{
					if (!it->is_array())
						throw CommandError("'extensions' must be an array of strings");
					for (const json& extension : *it)
					{
						if (!extension.is_string())
							throw CommandError("'extensions' must be an array of strings");
						extensions.push_back(extension.get<std::string>());
					}
				}
				return json{ { "assets", AssetManager::ListAssets(extensions) } };
			});

			Add(registry, "asset.write", "Writes a text asset (script, prefab, scene...) to a project-relative path.", { { "path", "string" }, { "content", "string" } }, [](AutomationSession&, const json& params) {
				RequireProject();
				const std::string path = ProjectRelative(RequireString(params, "path"));
				const json& content = Require(params, "content");
				if (!content.is_string())
					throw CommandError("'content' must be a string");
				if (!FileSystem::WriteTextFile(Project::ResolvePath(path), content.get<std::string>()))
					throw CommandError("cannot write '" + path + "'");
				AssetManager::Reload(path);
				return json{ { "path", path }, { "bytes", content.get<std::string>().size() } };
			});

			Add(registry, "asset.read", "Reads a text asset.", { { "path", "string" } }, [](AutomationSession&, const json& params) {
				const std::string path = ProjectRelative(RequireString(params, "path"));
				const auto text = FileSystem::ReadTextFile(Project::ResolvePath(path));
				if (!text)
					throw CommandError("cannot read '" + path + "'");
				return json{ { "path", path }, { "content", *text } };
			});

			Add(registry, "asset.import_model", "Instantiates a glTF model's node hierarchy into the scene.", { { "path", "string, .gltf/.glb or builtin://Cube" }, { "parent", "ID or name, optional" } }, [](AutomationSession& session, const json& params) {
				Scene& scene = RequireScene(session);
				Entity parent = params.contains("parent") && !params["parent"].is_null() ? ResolveEntity(scene, params["parent"], "parent") : Entity{};
				std::string error;
				Entity root = AssetManager::ImportModel(scene, RequireString(params, "path"), parent, error);
				if (!root)
					throw CommandError(error);
				MarkEdited(session);
				return EntitySummary(root);
			});

			Add(registry, "prefab.create", "Saves an entity tree as a prefab file.", { { "entity", "ID or name" }, { "path", "string, e.g. 'Assets/Prefabs/Enemy.bprefab'" } }, [](AutomationSession& session, const json& params) {
				Entity entity = RequireEntity(session, params);
				const std::string path = ProjectRelative(RequireString(params, "path"));
				std::string error;
				if (!SceneSerializer::SavePrefab(entity, Project::ResolvePath(path), error))
					throw CommandError(error);
				return json{ { "path", path } };
			});

			Add(registry, "prefab.instantiate", "Instantiates a prefab into the scene.", { { "path", "string" }, { "parent", "ID or name, optional" }, { "position", "[x, y, z], optional" } }, [](AutomationSession& session, const json& params) {
				Scene& scene = RequireScene(session);
				const std::string path = ProjectRelative(RequireString(params, "path"));
				Entity parent = params.contains("parent") && !params["parent"].is_null() ? ResolveEntity(scene, params["parent"], "parent") : Entity{};
				std::string error;
				Entity root = SceneSerializer::InstantiatePrefab(scene, Project::ResolvePath(path), path, parent, error);
				if (!root)
					throw CommandError(error);
				if (auto position = params.find("position"); position != params.end())
				{
					std::string patchError;
					if (!ComponentRegistry::AddOrPatch(root, "Transform", { { "Translation", *position } }, patchError))
						throw CommandError(patchError);
				}
				if (session.IsPlaying() && scene.GetScriptEngine())
				{
					std::vector<Entity> tree;
					for (Entity entity : scene.GetAllEntitiesOrdered())
					{
						if (entity == root || scene.IsDescendantOf(entity, root))
							tree.push_back(entity);
					}
					scene.GetScriptEngine()->EnsureInstances(tree);
				}
				MarkEdited(session);
				return EntitySummary(root);
			});

			Add(registry, "script.check", "Compiles a Lua script in isolation and returns its Properties, or the error.", { { "path", "string" } }, [](AutomationSession&, const json& params) {
				const std::string path = ProjectRelative(RequireString(params, "path"));
				std::string error;
				const auto properties = ScriptEngine::LoadScriptProperties(path, error);
				if (!properties)
					throw CommandError(error);
				return json{ { "properties", *properties } };
			});
		}

		void RegisterPlay(CommandRegistry& registry)
		{
			Add(registry, "play.start", "Starts play mode on a copy of the scene (scripts, physics, audio run).", {}, [](AutomationSession& session, const json&) {
				std::string error;
				if (!session.StartPlay(error))
					throw CommandError(error);
				return json{ { "playing", true } };
			});

			Add(registry, "play.stop", "Stops play mode and restores the edit scene.", {}, [](AutomationSession& session, const json&) {
				session.StopPlay();
				return json{ { "playing", false } };
			});

			Add(registry, "play.step", "Advances play mode by N fixed frames (default dt 1/60) and reports script errors.", { { "frames", "integer (default 1)" }, { "dt", "number, seconds per frame" }, { "assertNoErrors", "bool: fail the command if any script error occurred" }, { "hash", "bool: also return stateHash (see scene.hash)" } }, [](AutomationSession& session, const json& params) {
				if (!session.IsPlaying())
					throw CommandError("not playing (use play.start)");
				const uint32_t frames = static_cast<uint32_t>(OptionalInteger(params, "frames", 1, 1, 100000));
				float dt = 1.0f / 60.0f;
				if (auto it = params.find("dt"); it != params.end())
				{
					if (!JsonToFloat(*it, dt) || dt <= 0.0f || dt > 0.25f)
						throw CommandError("'dt' must be a number in (0, 0.25]");
				}
				session.Step(frames, dt);
				Scene& scene = RequireScene(session);
				json errors = scene.GetScriptEngine() ? json(scene.GetScriptEngine()->GetErrors()) : json::array();
				if (OptionalBool(params, "assertNoErrors", false) && !errors.empty())
					throw CommandError("script errors: " + errors.dump());
				json result = { { "time", scene.GetTime() }, { "frame", scene.GetFrameCount() }, { "scriptErrors", errors }, { "quitRequested", scene.IsQuitRequested() } };
				if (OptionalBool(params, "hash", false))
					result["stateHash"] = SceneSerializer::ComputeStateHash(scene);
				return result;
			});

			Add(registry, "replay.record_start", "Starts recording input.key/input.mouse commands with the play frame they arrive on.", {}, [](AutomationSession& session, const json&) {
				session.StartRecording();
				return json{ { "recording", true } };
			});

			Add(registry, "replay.record_stop", "Stops recording and writes the replay file (project-relative path, e.g. 'Replays/run.breplay').", { { "path", "string" } }, [](AutomationSession& session, const json& params) {
				RequireProject();
				const std::string path = ProjectRelative(RequireString(params, "path"));
				if (!session.IsRecording())
					throw CommandError("not recording (use replay.record_start)");
				const uint64_t frames = session.IsPlaying() ? RequireScene(session).GetFrameCount() : 0;
				const json events = session.StopRecording();
				json replay = { { "Format", "BasaltReplay" }, { "Version", 1 }, { "Frames", frames }, { "Events", events } };
				// The recorded run's final state: replay.run with assertHash checks a replay reproduces it.
				if (session.IsPlaying())
					replay["StateHash"] = SceneSerializer::ComputeStateHash(RequireScene(session));
				if (!FileSystem::WriteTextFile(Project::ResolvePath(path), replay.dump(1, '\t')))
					throw CommandError("cannot write '" + path + "'");
				return json{ { "path", path }, { "events", events.size() }, { "frames", frames } };
			});

			Add(registry, "replay.run", "Restarts play from the edit scene, re-applies a replay's inputs on their frames, and returns the state hash. Play keeps running afterwards for inspection.",
				{ { "path", "string, replay file" }, { "frames", "integer, default: the recorded length" }, { "assertNoErrors", "bool" }, { "assertHash", "bool: fail unless the final state hash equals the recorded one" } }, [](AutomationSession& session, const json& params) {
					RequireProject();
					const std::string path = ProjectRelative(RequireString(params, "path"));
					const auto text = FileSystem::ReadTextFile(Project::ResolvePath(path));
					if (!text)
						throw CommandError("cannot read '" + path + "'");
					const json replay = ParseJson(*text);
					if (!replay.is_object() || replay.value("Format", "") != "BasaltReplay" || !replay.contains("Events") || !replay["Events"].is_array())
						throw CommandError("'" + path + "' is not a Basalt replay file");
					const int64_t recorded = replay.value("Frames", int64_t{ 0 });
					const uint32_t frames = static_cast<uint32_t>(OptionalInteger(params, "frames", recorded, 0, 10000000));

					session.StopPlay(); // also resets input
					std::string error;
					if (!session.StartPlay(error))
						throw CommandError(error);
					const json& events = replay["Events"];
					size_t next = 0;
					for (uint32_t frame = 0; frame <= frames; frame++)
					{
						// Events recorded at frame N were applied before the update that produced frame N + 1.
						for (; next < events.size() && events[next].value("frame", uint64_t{ 0 }) <= frame; next++)
						{
							const json& event = events[next];
							const std::string command = event.value("command", "");
							const json eventParams = event.value("params", json::object());
							if (command == "input.key")
								ApplyKeyInput(eventParams);
							else if (command == "input.mouse")
								ApplyMouseInput(eventParams);
							else
								throw CommandError("replay event " + std::to_string(next) + " has unknown command '" + command + "'");
						}
						if (frame < frames)
							session.Step(1, 1.0f / 60.0f);
					}

					Scene& scene = RequireScene(session);
					json errors = scene.GetScriptEngine() ? json(scene.GetScriptEngine()->GetErrors()) : json::array();
					if (OptionalBool(params, "assertNoErrors", false) && !errors.empty())
						throw CommandError("script errors: " + errors.dump());
					const std::string hash = SceneSerializer::ComputeStateHash(scene);
					const std::string recordedHash = replay.value("StateHash", "");
					if (OptionalBool(params, "assertHash", false))
					{
						if (recordedHash.empty() || frames != static_cast<uint32_t>(recorded))
							throw CommandError("assertHash needs the full recorded length of a replay that stored a StateHash");
						if (hash != recordedHash)
							throw CommandError("replay diverged: state hash " + hash + ", recorded " + recordedHash);
					}
					return json{ { "frame", scene.GetFrameCount() }, { "stateHash", hash }, { "recordedHash", recordedHash }, { "scriptErrors", errors } };
				});

			Add(registry, "input.key", "Sets a key's state for play mode (e.g. {key: 'Space', down: true}). Recorded while replay recording is on.", { { "key", "string key name" }, { "down", "bool" } }, [](AutomationSession& session, const json& params) {
				ApplyKeyInput(params);
				session.RecordInput("input.key", params);
				return json::object();
			});

			Add(registry, "input.mouse", "Sets mouse position and/or a button state for play mode. Recorded while replay recording is on.", { { "position", "[x, y]" }, { "button", "string (Left, Right, Middle)" }, { "down", "bool" } }, [](AutomationSession& session, const json& params) {
				ApplyMouseInput(params);
				session.RecordInput("input.mouse", params);
				return json::object();
			});

			Add(registry, "lua.exec", "Runs Lua in the playing scene's script state; returns the value of the expression.", { { "code", "string" } }, [](AutomationSession& session, const json& params) {
				Scene& scene = RequireScene(session);
				if (!session.IsPlaying() || !scene.GetScriptEngine())
					throw CommandError("not playing (use play.start)");
				std::string result;
				if (!scene.GetScriptEngine()->ExecuteString(RequireString(params, "code"), result))
					throw CommandError(result);
				return json{ { "result", result } };
			});

			Add(registry, "render.screenshot", "Renders the current scene from its primary camera to a PNG (editor / GPU hosts only).", { { "path", "string, output .png" }, { "width", "integer" }, { "height", "integer" } }, [](AutomationSession& session, const json& params) {
				if (!session.Screenshot)
					throw CommandError("screenshots need a GPU host (the editor, or 'BasaltRuntime --frames N --screenshot out.png'); this host is '" + session.HostName + "'");
				const std::string path = RequireString(params, "path");
				const uint32_t width = static_cast<uint32_t>(OptionalInteger(params, "width", 1280, 16, 8192));
				const uint32_t height = static_cast<uint32_t>(OptionalInteger(params, "height", 720, 16, 8192));
				std::string error;
				if (!session.Screenshot(path, width, height, error))
					throw CommandError(error);
				return json{ { "path", path } };
			});

			// Headless, so tests and agents can check renders from any host (golden-image tests use it).
			Add(registry, "image.compare", "Compares a PNG with a reference image within a tolerance; optionally writes a diff image and fails on mismatch.",
				{ { "actual", "string, image path (relative paths are project-relative)" }, { "reference", "string, image path" }, { "diff", "string, output .png, optional" }, { "pixelThreshold", "integer 0-255, default 8" }, { "maxPercent", "number, default 0.5" }, { "assertMatch", "bool, default false" } },
				[](AutomationSession&, const json& params) {
					const std::filesystem::path actualPath = Project::ResolvePath(RequireString(params, "actual"));
					const std::filesystem::path referencePath = Project::ResolvePath(RequireString(params, "reference"));
					ImageCompareOptions options;
					options.PixelThreshold = static_cast<uint8_t>(OptionalInteger(params, "pixelThreshold", options.PixelThreshold, 0, 255));
					options.MaxDifferingPercent = OptionalNumber(params, "maxPercent", options.MaxDifferingPercent, 0.0, 100.0);
					const std::string diffParam = OptionalString(params, "diff");
					const std::filesystem::path diffPath = diffParam.empty() ? std::filesystem::path() : Project::ResolvePath(diffParam);

					std::string error;
					// The wording is stable: the golden-image ctests skip (not fail) on a missing reference.
					if (!std::filesystem::exists(referencePath))
						throw CommandError("reference image not found: '" + referencePath.string() + "'");
					Ref<TextureSource> reference = TextureSource::LoadFromFile(referencePath, error);
					if (!reference)
						throw CommandError("cannot load reference image: " + error);
					Ref<TextureSource> actual = TextureSource::LoadFromFile(actualPath, error);
					if (!actual)
						throw CommandError("cannot load image: " + error);

					ImageCompareResult result = CompareImages(*actual, *reference, options, !diffPath.empty());
					if (!diffPath.empty() && result.SizeMatches && !WritePng(diffPath, result.DiffImage, result.Width, result.Height, error))
						throw CommandError(error);

					json response = {
						{ "matches", result.Matches },
						{ "sizeMatches", result.SizeMatches },
						{ "size", { actual->Width, actual->Height } },
						{ "referenceSize", { reference->Width, reference->Height } },
						{ "maxDelta", result.MaxDelta },
						{ "meanDelta", result.MeanDelta },
						{ "differingPercent", result.DifferingPercent },
					};
					if (OptionalBool(params, "assertMatch", false) && !result.Matches)
						throw CommandError("images differ: " + response.dump());
					return response;
				});
		}

	}

	void RegisterBuiltinCommands(CommandRegistry& registry)
	{
		RegisterGeneral(registry);
		RegisterProject(registry);
		RegisterScene(registry);
		RegisterEntity(registry);
		RegisterComponents(registry);
		RegisterAssets(registry);
		RegisterPlay(registry);
	}

}
