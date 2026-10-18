#include "Basalt/Animation/AnimatedModel.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Asset/ImageUtils.h"
#include "Basalt/Asset/TextureSource.h"
#include "Basalt/Automation/AutomationEvents.h"
#include "Basalt/Automation/AutomationSession.h"
#include "Basalt/Animation/AnimationGraph.h"
#include "Basalt/Automation/CommandRegistry.h"
#include "Basalt/Automation/CommandSchema.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Input.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/MemoryTracker.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Project/Exporter.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Project/ProjectValidator.h"
#include "Basalt/Project/SmokeTest.h"
#include "Basalt/Renderer/RenderMath.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scene/SceneDiff.h"
#include "Basalt/Scene/SceneSerializer.h"
#include "Basalt/Scripting/ScriptEngine.h"
#include "Basalt/UI/GameUI.h"
#include "Basalt/UI/UIFonts.h"
#include "Basalt/UI/UIInteraction.h"
#include "Basalt/UI/UILayout.h"

#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace Basalt {

	namespace {

		using json = nlohmann::json;
		namespace Schema = CommandSchema;

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

		// component.types' description of a field list: names, types and enum options, plus each list field's
		// element type described the same way (recursively).
		json DescribeFields(const std::vector<std::string>& fields, const std::map<std::string, ComponentFieldType>& fieldTypes, const std::map<std::string, std::vector<std::string>>& enumOptions)
		{
			json types = json::object();
			json lists = json::object();
			for (const auto& [field, type] : fieldTypes)
			{
				types[field] = type.ToString();
				if (type.Kind != ComponentFieldKind::List)
					continue;
				const ComponentObjectSchema& element = *type.Element;
				json list = DescribeFields(element.Fields, element.FieldTypes, element.EnumOptions);
				list["name"] = element.Name;
				list["maxCount"] = type.MaxCount;
				if (!element.UniqueField.empty())
					list["uniqueField"] = element.UniqueField;
				list["default"] = element.Default;
				lists[field] = std::move(list);
			}
			return { { "fields", fields }, { "types", types }, { "enums", enumOptions }, { "lists", lists } };
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

		// Whether an optional parameter was given (absent and null mean "not given", as for the Optional* helpers).
		bool HasParam(const json& params, const char* name)
		{
			auto it = params.find(name);
			return it != params.end() && !it->is_null();
		}

		const PhysicsLayers& ActivePhysicsLayers()
		{
			static const PhysicsLayers s_DefaultLayers;
			const Ref<Project>& project = Project::GetActive();
			return project ? project->GetConfig().Physics : s_DefaultLayers;
		}

		// Entities on layers the project does not define (they play on Default), as [{entity, name, layer}].
		json UnknownPhysicsLayersJson(Scene& scene, const PhysicsLayers& layers)
		{
			json result = json::array();
			for (const UnknownPhysicsLayerUse& use : FindUnknownPhysicsLayers(scene, layers))
				result.push_back({ { "entity", static_cast<uint64_t>(use.Entity) }, { "name", use.EntityName }, { "layer", use.Layer } });
			return result;
		}

		json ValidationReportJson(const ValidationReport& report)
		{
			json issues = json::array();
			for (const ValidationIssue& issue : report.GetIssues())
				issues.push_back({ { "severity", issue.Severity == ValidationSeverity::Error ? "error" : "warning" }, { "file", issue.File }, { "message", issue.Message } });
			return { { "valid", report.IsValid() }, { "errors", report.GetErrorCount() }, { "warnings", report.GetWarningCount() }, { "omitted", report.GetOmittedCount() }, { "issues", issues } };
		}

		json SmokeTestJson(const SmokeTestResult& result)
		{
			static constexpr size_t LogLines = 40;
			const char* outcome = "failed";
			json exitCode;
			switch (result.Process.Outcome)
			{
				case ProcessOutcome::Exited:
					outcome = "exited";
					exitCode = result.Process.ExitCode;
					break;
				case ProcessOutcome::Crashed:
					outcome = "crashed";
					exitCode = result.Process.ExitCode;
					break;
				case ProcessOutcome::TimedOut:
					outcome = "timedOut";
					break;
				case ProcessOutcome::OutputLimit:
					outcome = "outputLimit";
					break;
				case ProcessOutcome::Failed:
					break;
			}
			return { { "passed", result.Passed() }, { "outcome", outcome }, { "exitCode", exitCode }, { "summary", result.Summary }, { "log", LastLines(result.Process.Output, LogLines) } };
		}

		constexpr int64_t DefaultSmokeTestFrames = 120;
		constexpr int64_t MaxSmokeTestFrames = 100000;
		constexpr double DefaultSmokeTestSeconds = 60.0;
		constexpr double MaxSmokeTestSeconds = 3600.0;

		std::chrono::milliseconds SecondsToMilliseconds(double seconds)
		{
			return std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000.0));
		}

		// The first few errors of a failed validation, for a command error message.
		std::string DescribeValidationErrors(const ValidationReport& report)
		{
			static constexpr size_t Listed = 5;
			std::string text = fmt::format("project validation found {} error(s)", report.GetErrorCount());
			size_t listed = 0;
			for (const ValidationIssue& issue : report.GetIssues())
			{
				if (listed == Listed)
					break;
				if (issue.Severity != ValidationSeverity::Error)
					continue;
				text += "\n" + issue.File + ": " + issue.Message;
				listed++;
			}
			if (report.GetErrorCount() > Listed)
				text += "\n(run project.validate for the full list)";
			return text;
		}

		std::string SmokeTestFailure(const SmokeTestResult& result)
		{
			static constexpr size_t LogLines = 15;
			return "smoke test failed: " + result.Summary + "\n" + LastLines(result.Process.Output, LogLines);
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

		// Every entity a reference names: the one with a numeric ID, or all with a name. outDescription reads
		// "with ID 5" or "named 'Door'", for messages.
		std::vector<Entity> FindEntities(Scene& scene, const json& reference, const char* parameter, std::string& outDescription)
		{
			if (reference.is_number_unsigned() || reference.is_number_integer())
			{
				outDescription = "with ID " + std::to_string(reference.get<uint64_t>());
				Entity entity = scene.GetEntityByUUID(reference.get<uint64_t>());
				return entity ? std::vector<Entity>{ entity } : std::vector<Entity>{};
			}
			if (reference.is_string())
			{
				outDescription = "named '" + reference.get<std::string>() + "'";
				return scene.FindEntitiesByName(reference.get<std::string>());
			}
			throw CommandError(std::string("parameter '") + parameter + "' must be an entity ID (number) or name (string)");
		}

		// Entities are addressed by numeric ID or by (unique) name.
		Entity ResolveEntity(Scene& scene, const json& reference, const char* parameter = "entity")
		{
			std::string description;
			const std::vector<Entity> matches = FindEntities(scene, reference, parameter, description);
			if (matches.empty())
				throw CommandError("no entity " + description);
			if (matches.size() > 1)
				throw CommandError("several entities are " + description + "; use the numeric ID");
			return matches.front();
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

		const ComponentInfo& RequireComponentInfo(const std::string& name)
		{
			const ComponentInfo* info = ComponentRegistry::Find(name);
			if (!info)
				throw CommandError("unknown component '" + name + "' (valid: " + ComponentRegistry::GetNameList() + ")");
			return *info;
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

		// Makes a just loaded or created project the active one, starting its assets and file watching afresh.
		void ActivateProject(AutomationSession& session, const Ref<Project>& project)
		{
			Project::SetActive(project);
			session.SyncFileWatcher();
			AssetManager::Clear();
			PhysicsWorld::ClearMeshShapeCache();
		}

		// The script part of an asset reload, as asset.write and asset.refresh return it.
		json ScriptReloadJson(const AssetReloadReport& report)
		{
			json failed = json::array();
			for (const ScriptReloadFailure& failure : report.Scripts.Failed)
				failed.push_back({ { "path", failure.Path }, { "error", failure.Error } });
			return { { "hotReload", ScriptHotReloadName(report.HotReload) }, { "reloaded", report.Scripts.Reloaded }, { "failed", failed }, { "instances", report.Scripts.Instances }, { "instanceErrors", report.Scripts.InstanceErrors }, { "error", report.Scripts.Error } };
		}

		void Add(CommandRegistry& registry, std::string name, std::string description, json inputSchema, std::function<json(AutomationSession&, const json&)> handler, CommandPolicy policy = CommandPolicy::Normal)
		{
			registry.Register({ std::move(name), std::move(description), std::move(inputSchema), std::move(handler), policy });
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

		// ui.layout's description of a widget entity.
		json WidgetSummary(const Scene& scene, Entity entity, const UIWidgetInfo& info)
		{
			json summary = { { "kind", UIWidgetComponentName(info.Kind) }, { "interactable", info.Interactable }, { "target", static_cast<uint64_t>(info.Target) }, { "callback", info.Callback } };
			if (info.Kind == UIWidgetKind::Slider)
				summary["value"] = entity.GetComponent<UISliderComponent>().Value;
			else if (info.Kind == UIWidgetKind::Checkbox)
				summary["checked"] = entity.GetComponent<UICheckboxComponent>().Checked;
			if (const UIInteraction* interaction = scene.GetUIInteraction())
			{
				const UIWidgetID id = UIWidgetID::ForEntity(entity.GetUUID());
				summary["state"] = UIWidgetStateName(interaction->GetState(id, info.Interactable));
				summary["focused"] = interaction->GetFocus() == id;
			}
			return summary;
		}

		// Why a widget's callback cannot run as configured (empty when it can, or it has none). A missing method is
		// only found when it is called, since the script is not loaded here.
		std::string WidgetCallbackProblem(Scene& scene, Entity entity, const UIWidgetInfo& info)
		{
			// The registry rejects invalid names, so only C++ code writes one; the widget then calls nothing.
			if (info.AuthoredCallback != info.Callback)
				return fmt::format("{} callback '{}' is not a script method name (it calls nothing)", UIWidgetComponentName(info.Kind), info.AuthoredCallback);
			if (info.Callback.empty())
				return {};
			const char* component = UIWidgetComponentName(info.Kind);
			Entity target = scene.GetEntityByUUID(info.ResolveTarget(entity.GetUUID()));
			if (!target)
				return fmt::format("{} Target {} does not exist (its callback cannot run)", component, static_cast<uint64_t>(info.Target));
			if (!target.HasComponent<ScriptComponent>())
				return fmt::format("{} calls {} on '{}', which has no Script", component, info.Callback, target.GetName());
			return {};
		}

		// A replay's "view" event (AutomationSession::RecordInput): the host view the recording had from then on.
		void ApplyViewEvent(const json& params)
		{
			const auto readVec2 = [&params](const char* name, glm::vec2& out) {
				const auto it = params.find(name);
				return it != params.end() && it->is_array() && it->size() == 2 && JsonToFloat((*it)[0], out.x) && JsonToFloat((*it)[1], out.y);
			};
			const auto readSize = [&readVec2](const char* name, glm::vec2& out) {
				if (!readVec2(name, out) || !(out.x > 0.0f && out.y > 0.0f))
					throw CommandError(fmt::format("'{}' must be [width, height] with positive values", name));
			};
			GameUI::HostState view;
			if (!readVec2("origin", view.ViewportOrigin))
				throw CommandError("'origin' must be [x, y]");
			readSize("size", view.ViewportSize);
			view.PointerEnabled = OptionalBool(params, "pointerEnabled", true);
			view.NavigationEnabled = OptionalBool(params, "navigationEnabled", true);
			// Recordings made before the Game view mapped the mouse have none: the viewport size, as they ran.
			if (params.contains("resolution") && !params["resolution"].is_null())
			{
				glm::vec2 resolution;
				readSize("resolution", resolution);
				view.Resolution = resolution;
			}
			GameUI::SetHostState(view);
		}

		// Validates the whole request before changing anything, so a bad one leaves the gamepad as it was (and is not
		// recorded half-applied).
		void ApplyGamepadInput(const json& params)
		{
			constexpr size_t MaxNameLength = 256;
			const auto index = static_cast<uint32_t>(OptionalInteger(params, "index", 0, 0, Input::MaxGamepads - 1));
			const bool hasConnected = params.contains("connected");
			const bool hasButton = params.contains("button");
			const bool hasAxis = params.contains("axis");
			if (!hasConnected && !hasButton && !hasAxis)
				throw CommandError("give 'connected', 'button' or 'axis'");
			// Plain bools rather than optional<bool>: GCC's -Wmaybe-uninitialized misfires on optional<bool> in Release.
			const bool connect = hasConnected && OptionalBool(params, "connected", true);
			const bool disconnect = hasConnected && !connect;
			if (params.contains("name") && !connect)
				throw CommandError("'name' names a gamepad being connected: pass it with connected: true");
			if (params.contains("down") && !hasButton)
				throw CommandError("'down' goes with 'button'");
			if (params.contains("value") && !hasAxis)
				throw CommandError("'value' goes with 'axis'");
			const std::string name = params.contains("name") ? RequireString(params, "name") : std::string(Input::DefaultInjectedGamepadName);
			if (name.size() > MaxNameLength)
				throw CommandError(fmt::format("'name' must be at most {} characters", MaxNameLength));
			std::optional<GamepadButton> button;
			const bool down = OptionalBool(params, "down", true);
			if (hasButton)
			{
				const std::string buttonName = RequireString(params, "button");
				button = GamepadButtonFromString(buttonName);
				if (!button)
					throw CommandError(fmt::format("unknown gamepad button '{}' (valid: {})", buttonName, fmt::join(GetGamepadButtonNames(), ", ")));
			}
			std::optional<GamepadAxis> axis;
			float value = 0.0f;
			if (hasAxis)
			{
				const std::string axisName = RequireString(params, "axis");
				axis = GamepadAxisFromString(axisName);
				if (!axis)
					throw CommandError(fmt::format("unknown gamepad axis '{}' (valid: {})", axisName, fmt::join(GetGamepadAxisNames(), ", ")));
				const auto found = params.find("value");
				const float min = IsGamepadTrigger(*axis) ? 0.0f : -1.0f;
				if (found == params.end() || !JsonToFloat(*found, value) || value < min || value > 1.0f)
					throw CommandError(fmt::format("'value' must be a number from {} to 1 for axis '{}'", min, axisName));
			}
			// A physical gamepad is the device's: the next poll would undo what a command changed.
			const GamepadSource source = Input::GetGamepadSource(index);
			if (source == GamepadSource::Device && disconnect)
				throw CommandError(fmt::format("gamepad {} is a physical device and cannot be disconnected (inject at another index)", index));
			if (source == GamepadSource::Device && (button || axis) && !connect)
				throw CommandError(fmt::format("gamepad {} is a physical device; pass connected: true to take it over", index));
			if ((button || axis) && !connect && (disconnect || source == GamepadSource::None))
				throw CommandError(fmt::format("gamepad {} is not connected (pass connected: true)", index));

			if (hasConnected)
				Input::SetGamepadConnected(index, connect, name);
			// The checks above make the gamepad injected and connected here; a failure would record an input that never
			// happened, so it is an error even where asserts are off.
			if (button && !Input::SetGamepadButtonState(index, *button, down))
				throw CommandError(fmt::format("gamepad {} rejected the button", index));
			if (axis && !Input::SetGamepadAxis(index, *axis, value))
				throw CommandError(fmt::format("gamepad {} rejected the axis", index));
		}

		// --- Command groups -------------------------------------------------------------------------

		void RegisterGeneral(CommandRegistry& registry)
		{
			Add(registry, "help", "Lists commands, or describes one command with its parameter schema.", Schema::Object({ { "command", Schema::String("Command name to describe", true) } }), [&registry](AutomationSession&, const json& params) {
				const auto describe = [](const CommandInfo& info) {
					json parameters = json::object();
					for (const auto& [name, schema] : info.InputSchema["properties"].items())
						parameters[name] = schema.value("description", "");
					return json{ { "command", info.Name }, { "description", info.Description }, { "params", parameters }, { "inputSchema", info.InputSchema } };
				};
				if (params.contains("command"))
				{
					const auto* info = registry.Find(params["command"].get<std::string>());
					if (!info)
						throw CommandError("unknown command '" + params["command"].get<std::string>() + "'");
					return describe(*info);
				}
				json commands = json::array();
				for (const auto& [name, info] : registry.GetCommands())
					commands.push_back(describe(info));
				return json{ { "commands", commands } };
			});

			Add(registry, "component.types", "Lists every component type with its fields (for component.set): their names in order, each field's JSON type ('number', 'integer', 'boolean', 'string', 'object', 'entity', 'enum', an array such as 'number[3]', or 'object[]' for a list of objects), the values of enum fields, the fields that reference entities, and for each list field its element type under 'lists' ({name, fields, types, enums, maxCount, default, lists, and uniqueField when one string field must differ between elements}; a set replaces the whole list).", Schema::Object(), [](AutomationSession&, const json&) {
				json types = json::array();
				for (const ComponentInfo& info : ComponentRegistry::GetAll())
				{
					json description = DescribeFields(info.Fields, info.FieldTypes, info.EnumOptions);
					description["name"] = info.Name;
					description["entityFields"] = info.EntityFields;
					description["core"] = info.IsCore;
					types.push_back(std::move(description));
				}
				return json{ { "components", types } };
			});

			Add(registry, "log.get", "Returns engine log messages logged after index 'since'.", Schema::Object({ { "since", Schema::Nullable(Schema::Integer("integer, cursor from a previous call (default 0)", 0, INT64_MAX)) }, { "errorsOnly", Schema::Nullable(Schema::Boolean("bool")) } }), [](AutomationSession&, const json& params) {
				const uint64_t since = static_cast<uint64_t>(OptionalInteger(params, "since", 0, 0, INT64_MAX));
				const bool errorsOnly = OptionalBool(params, "errorsOnly", false);
				uint64_t next = 0;
				json messages = json::array();
				for (const LogMessage& message : Log::GetHistory().GetMessagesSince(since, next))
				{
					if (errorsOnly && message.Level < LogLevel::Error)
						continue;
					messages.push_back({ { "level", LogLevelName(message.Level) }, { "logger", message.LoggerName }, { "text", message.Text } });
				}
				return json{ { "messages", messages }, { "next", next } };
			});
		}

		// Fails when params has an expectHash that differs from the state hash; the message names both so a
		// failing CI run shows each platform's value. A present but empty or null expectHash is an error, not a
		// check that silently passes.
		void CheckExpectedHash(const json& params, const std::string& hash)
		{
			if (!params.contains("expectHash"))
				return;
			const std::string expected = RequireString(params, "expectHash");
			if (expected != hash)
				throw CommandError("state hash " + hash + " differs from the expected " + expected);
		}

		void RegisterProject(CommandRegistry& registry)
		{
			Add(registry, "project.create", "Creates a new project directory (Project.bproject + Assets/) and opens it.", Schema::Object({ { "path", Schema::String("string, project directory", true) }, { "name", Schema::Nullable(Schema::String("string")) } }, { "path" }), [](AutomationSession& session, const json& params) {
				std::string error;
				Ref<Project> project = Project::Create(RequireString(params, "path"), OptionalString(params, "name", "Game"), error);
				if (!project)
					throw CommandError(error);
				ActivateProject(session, project);
				session.SetEditScene(CreateRef<Scene>("Untitled"), "");
				return json{ { "directory", project->GetDirectory().string() } }; }, CommandPolicy::EditBoundary);

			Add(registry, "project.open", "Opens a project (directory or .bproject file) and its start scene.", Schema::Object({ { "path", Schema::String("string", true) } }, { "path" }), [](AutomationSession& session, const json& params) {
				std::string error;
				Ref<Project> project = Project::Load(RequireString(params, "path"), error);
				if (!project)
					throw CommandError(error);
				ActivateProject(session, project);
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
				return json{ { "name", project->GetConfig().Name }, { "scene", startScene } }; }, CommandPolicy::EditBoundary);

			Add(registry, "project.info", "Returns the active project's settings.", Schema::Object(), [](AutomationSession&, const json&) {
				Ref<Project> project = RequireProject();
				const ProjectConfig& config = project->GetConfig();
				return json{
					{ "name", config.Name },
					{ "directory", project->GetDirectory().string() },
					{ "startScene", config.StartScene },
					{ "windowWidth", config.WindowWidth },
					{ "windowHeight", config.WindowHeight },
					{ "fullscreen", config.Fullscreen },
					{ "graphics", SerializeGraphicsSettings(Project::GetActiveGraphicsSettings()) },
					{ "renderScale", config.Graphics.RenderScale },
					{ "dynamicResolution", config.Graphics.DynamicResolution.Enabled },
					{ "targetFrameRate", config.Graphics.DynamicResolution.TargetFrameRate },
					{ "minRenderScale", config.Graphics.DynamicResolution.MinScale },
					{ "physicsLayers", config.Physics.ToJson() },
					{ "randomSeed", config.RandomSeed },
					{ "fixedTimestep", config.FixedTimestep },
					{ "inputActions", config.InputActions.ToJson() },
				};
			});

			json graphicsSchema = Schema::Object({ { "VSync", Schema::Boolean("Presentation synchronization") },
												   { "RenderScale", Schema::Number("resolution scale", MinRenderScale, MaxRenderScale) },
												   { "ShadowQuality", json{ { "type", "string" }, { "enum", GraphicsQualityNames }, { "description", "Shadow quality cap" } } },
												   { "SSAOQuality", json{ { "type", "string" }, { "enum", GraphicsQualityNames }, { "description", "SSAO quality cap" } } },
												   { "DynamicResolution", Schema::Boolean("Enable dynamic resolution") },
												   { "TargetFrameRate", Schema::Number("target FPS", MinTargetFrameRate, MaxTargetFrameRate) },
												   { "MinRenderScale", Schema::Number("minimum scale", MinRenderScale, MaxRenderScale) } });
			graphicsSchema["description"] = "Project graphics defaults; applies to the next Play session. Do not repeat a setting through its top-level alias.";
			Add(registry, "project.set", "Changes project settings and saves the project file.", Schema::Object({ { "graphics", Schema::Nullable(graphicsSchema) }, { "name", Schema::Nullable(Schema::String("string")) }, { "startScene", Schema::Nullable(Schema::String("string, project-relative scene")) }, { "windowWidth", Schema::Nullable(Schema::Integer("integer", ProjectConfig::MinWindowSize, ProjectConfig::MaxWindowSize)) }, { "windowHeight", Schema::Nullable(Schema::Integer("integer", ProjectConfig::MinWindowSize, ProjectConfig::MaxWindowSize)) }, { "fullscreen", Schema::Nullable(Schema::Boolean("bool")) }, { "renderScale", Schema::Nullable(Schema::Number(fmt::format("number in [{}, {}], fraction of the window resolution the scene renders at (the highest scale with dynamic resolution)", MinRenderScale, MaxRenderScale), MinRenderScale, MaxRenderScale)) }, { "dynamicResolution", Schema::Nullable(Schema::Boolean("bool, lower the render scale when the GPU cannot hold targetFrameRate (needs GPU timer queries)")) }, { "targetFrameRate", Schema::Nullable(Schema::Number(fmt::format("number in [{}, {}]; dynamic resolution keeps the scene's GPU time within this frame rate's frame time", MinTargetFrameRate, MaxTargetFrameRate), MinTargetFrameRate, MaxTargetFrameRate)) }, { "minRenderScale", Schema::Nullable(Schema::Number(fmt::format("number in [{}, {}], lowest scale dynamic resolution uses; it must be below renderScale, or dynamic resolution is inactive (a warning)", MinRenderScale, MaxRenderScale), MinRenderScale, MaxRenderScale)) }, { "physicsLayers", json{ { "type", "object" }, { "description", "object {Names: [\"Default\", ...], IgnoredPairs: [[a, b], ...]}, replaces all layers; applies from the next play" } } }, { "randomSeed", Schema::Nullable(Schema::Integer("integer in [0, 4294967295], seeds Lua's random numbers, particle emitters and runtime entity UUIDs; applies from the next play", 0, UINT32_MAX)) }, { "fixedTimestep", Schema::Nullable(json{ { "type", "number" }, { "description", fmt::format("number of seconds in (0, {}], the physics step of every scene that does not override it (scene.settings Physics.FixedTimestep); applies from the next play", MaxFixedTimestep) }, { "exclusiveMinimum", 0.0 }, { "maximum", MaxFixedTimestep } }) }, { "inputActions", json{ { "type", "object" }, { "description", "object {Actions: [{Name, Type: \"Button\" | \"Axis\", Bindings: [{Key | MouseButton | GamepadButton | GamepadAxis: name, Scale?, DeadZone?, Gamepad?}]}]}, replaces every input action and saves Input.binput (written only when the actions change); applies from the next play" } } } }), [](AutomationSession& session, const json& params) {
				Ref<Project> project = RequireProject();
				// Every value is read and validated before any is assigned, so a bad parameter leaves the
				// project untouched.
				ProjectConfig config = project->GetConfig();
				config.Name = OptionalString(params, "name", config.Name);
				config.StartScene = OptionalString(params, "startScene", config.StartScene);
				config.WindowWidth = static_cast<uint32_t>(OptionalInteger(params, "windowWidth", config.WindowWidth, ProjectConfig::MinWindowSize, ProjectConfig::MaxWindowSize));
				config.WindowHeight = static_cast<uint32_t>(OptionalInteger(params, "windowHeight", config.WindowHeight, ProjectConfig::MinWindowSize, ProjectConfig::MaxWindowSize));
				config.Fullscreen = OptionalBool(params, "fullscreen", config.Fullscreen);
				config.Graphics.RenderScale = static_cast<float>(OptionalNumber(params, "renderScale", config.Graphics.RenderScale, MinRenderScale, MaxRenderScale));
				config.Graphics.DynamicResolution.Enabled = OptionalBool(params, "dynamicResolution", config.Graphics.DynamicResolution.Enabled);
				config.Graphics.DynamicResolution.TargetFrameRate = static_cast<float>(OptionalNumber(params, "targetFrameRate", config.Graphics.DynamicResolution.TargetFrameRate, MinTargetFrameRate, MaxTargetFrameRate));
				config.Graphics.DynamicResolution.MinScale = static_cast<float>(OptionalNumber(params, "minRenderScale", config.Graphics.DynamicResolution.MinScale, MinRenderScale, MaxRenderScale));
				config.RandomSeed = static_cast<uint32_t>(OptionalInteger(params, "randomSeed", config.RandomSeed, 0, UINT32_MAX));
				config.FixedTimestep = static_cast<float>(OptionalNumber(params, "fixedTimestep", config.FixedTimestep, 0.0, MaxFixedTimestep));
				if (!IsValidFixedTimestep(config.FixedTimestep))
					throw CommandError(fmt::format("parameter 'fixedTimestep' must be in (0, {}]", MaxFixedTimestep));
				if (const auto it = params.find("physicsLayers"); it != params.end())
				{
					std::string error;
					auto layers = PhysicsLayers::FromJson(*it, error);
					if (!layers)
						throw CommandError("physicsLayers: " + error);
					config.Physics = std::move(*layers);
				}
				if (const auto it = params.find("inputActions"); it != params.end())
				{
					std::string error;
					auto actions = InputActionMap::FromJson(*it, error);
					if (!actions)
						throw CommandError("inputActions: " + error);
					config.InputActions = std::move(*actions);
				}
				if (const auto it = params.find("graphics"); it != params.end() && !it->is_null())
				{
					for (const auto& [alias, key] : std::initializer_list<std::pair<const char*, const char*>>{
							 { "renderScale", "RenderScale" }, { "dynamicResolution", "DynamicResolution" }, { "targetFrameRate", "TargetFrameRate" }, { "minRenderScale", "MinRenderScale" } })
						if (params.contains(alias) && !params[alias].is_null() && it->contains(key))
							throw CommandError(fmt::format("graphics: specify either {} or graphics.{}, not both", alias, key));
					std::string error;
					if (!PatchGraphicsSettings(*it, config.Graphics, error))
						throw CommandError("graphics: " + error);
				}

				// A save that fails leaves the settings as they were, so what is shown is what is saved.
				ProjectConfig previous = std::exchange(project->GetConfig(), std::move(config));
				std::string error;
				bool saveSucceeded = false;
				try
				{
					saveSucceeded = project->Save(error);
				}
				catch (const std::exception& e)
				{
					error = e.what();
				}
				if (!saveSucceeded)
				{
					project->GetConfig() = std::move(previous);
					throw CommandError(error);
				}
				json result = json::object();
				// Valid but useless, and a headless host has no renderer to warn about it later.
				const ProjectConfig& saved = project->GetConfig();
				const DynamicResolutionState dynamicState = GetDynamicResolutionState(saved.Graphics.DynamicResolution, saved.Graphics.RenderScale, true);
				json warnings = json::array();
				if (dynamicState == DynamicResolutionState::EmptyRange)
					warnings.push_back("dynamic resolution " + DescribeDynamicResolutionState(dynamicState, saved.Graphics.DynamicResolution, saved.Graphics.RenderScale));
				// The project step does not reach a scene with its own; say so rather than appear to do nothing.
				if (const Scene* scene = session.GetScene().get(); params.contains("fixedTimestep") && scene && scene->GetPhysicsSettings().HasFixedTimestepOverride())
					warnings.push_back(fmt::format("the open scene keeps its own physics step ({} s); remove it with scene.settings {{\"Physics\": {{\"FixedTimestep\": null}}}}", scene->GetPhysicsSettings().FixedTimestep));
				if (!warnings.empty())
					result["warnings"] = warnings;
				// Renaming or removing a layer strands bodies on it; report them now rather than at play time.
				if (session.GetScene())
					result["unknownPhysicsLayers"] = UnknownPhysicsLayersJson(*session.GetScene(), project->GetConfig().Physics);
				return result;
			});

			Add(registry, "project.validate", "Checks that the project would export and start: settings, start scene, every scene and prefab (loads; referenced asset files exist inside Assets/ with the exact letter case; path fields must name files inside Assets/ with forward slashes; other component text naming a missing Assets/ file is a warning), every Lua script (compiles) and the scripts Script components use (load as script classes). Returns {valid, errors, warnings, omitted, issues: [{severity, file, message}]}.", Schema::Object({ { "assertValid", Schema::Nullable(Schema::Boolean("bool: fail the command when there is an error-severity issue")) } }), [](AutomationSession&, const json& params) {
				const ValidationReport report = ValidateProject(*RequireProject());
				if (OptionalBool(params, "assertValid", false) && !report.IsValid())
					throw CommandError(DescribeValidationErrors(report));
				return ValidationReportJson(report);
			});

			Add(registry, "project.export", "Validates the project (unless validate is false), exports a standalone, distributable build of the game (runtime + assets) and optionally smoke-tests it: runs the exported game headless for smokeTestFrames frames and fails unless it exits cleanly without script errors.", Schema::Object({ { "output", Schema::String("string, output directory", true) }, { "validate", Schema::Nullable(Schema::Boolean("bool (default true): refuse to export when project.validate finds an error")) }, { "smokeTestFrames", Schema::Nullable(Schema::Integer("integer (default 0: no smoke test), frames the exported game runs headless", 0, MaxSmokeTestFrames)) }, { "smokeTestTimeout", Schema::Nullable(Schema::Number("number of seconds (default 60) before the smoke test is stopped and fails", 1, MaxSmokeTestSeconds)) } }, { "output" }), [](AutomationSession& session, const json& params) {
				Ref<Project> project = RequireProject();
				if (session.IsDirty())
					throw CommandError("the open scene has unsaved changes; run scene.save first");
				const int64_t smokeFrames = OptionalInteger(params, "smokeTestFrames", 0, 0, MaxSmokeTestFrames);
				const double smokeSeconds = OptionalNumber(params, "smokeTestTimeout", DefaultSmokeTestSeconds, 1, MaxSmokeTestSeconds);
				if (smokeFrames == 0 && params.contains("smokeTestTimeout") && !params["smokeTestTimeout"].is_null())
					throw CommandError("smokeTestTimeout needs smokeTestFrames (the number of frames the smoke test runs)");
				json validation;
				if (OptionalBool(params, "validate", true))
				{
					const ValidationReport report = ValidateProject(*project);
					if (!report.IsValid())
						throw CommandError("not exported: " + DescribeValidationErrors(report));
					validation = ValidationReportJson(report);
				}
				ExportResult result = Exporter::Export(*project, RequireString(params, "output"));
				if (!result.Success)
					throw CommandError(result.Error);
				json response{ { "executable", FileSystem::PathToUtf8(result.ExecutablePath) }, { "files", result.FileCount }, { "validation", validation } };
				if (smokeFrames > 0)
				{
					const SmokeTestResult smoke = RunSmokeTest(result.ExecutablePath, static_cast<uint32_t>(smokeFrames), SecondsToMilliseconds(smokeSeconds));
					if (!smoke.Passed())
						throw CommandError("exported to '" + FileSystem::PathToUtf8(result.ExecutablePath.parent_path()) + "', but the " + SmokeTestFailure(smoke));
					response["smokeTest"] = SmokeTestJson(smoke);
				}
				return response;
			});

			Add(registry, "project.smoke_test", "Runs an exported game headless (no GPU needed) for up to N frames at the runtime's fixed 1/60 s step and reports whether it exited cleanly: exit code 0 and no script errors within the timeout. Returns {passed, outcome: exited|crashed|timedOut|outputLimit (stopped after 256 MiB of output)|failed, exitCode (null unless it exited or crashed), summary, log}.", Schema::Object({ { "executable", Schema::String("string, the exported game's executable (project.export's executable); a relative path is relative to the current working directory", true) }, { "frames", Schema::Nullable(Schema::Integer("integer (default 120)", 1, MaxSmokeTestFrames)) }, { "timeout", Schema::Nullable(Schema::Number("number of seconds (default 60)", 1, MaxSmokeTestSeconds)) }, { "assertPassed", Schema::Nullable(Schema::Boolean("bool: fail the command when the smoke test fails")) } }, { "executable" }), [](AutomationSession&, const json& params) {
				// Relative paths resolve against the working directory of the process running the command.
				const std::filesystem::path executable = FileSystem::PathFromUtf8(RequireString(params, "executable"));
				std::error_code error;
				const bool isFile = std::filesystem::is_regular_file(executable, error);
				if (error && error != std::errc::no_such_file_or_directory)
					throw CommandError("cannot check '" + FileSystem::PathToUtf8(executable) + "': " + error.message());
				if (!isFile)
					throw CommandError("no executable at '" + FileSystem::PathToUtf8(executable) + "'");
				const auto frames = static_cast<uint32_t>(OptionalInteger(params, "frames", DefaultSmokeTestFrames, 1, MaxSmokeTestFrames));
				const SmokeTestResult smoke = RunSmokeTest(executable, frames, SecondsToMilliseconds(OptionalNumber(params, "timeout", DefaultSmokeTestSeconds, 1, MaxSmokeTestSeconds)));
				if (OptionalBool(params, "assertPassed", false) && !smoke.Passed())
					throw CommandError(SmokeTestFailure(smoke));
				return SmokeTestJson(smoke);
			});
		}

		// A scene state named by a scene.diff parameter: a snapshot, else a .bscene file. Files are loaded and
		// serialized again so older file versions compare in the current format.
		json LoadSceneState(AutomationSession& session, const std::string& reference)
		{
			if (const json* snapshot = session.FindSnapshot(reference))
				return *snapshot;
			if (!reference.ends_with(".bscene"))
				throw CommandError("no snapshot named '" + reference + "' (take one with scene.snapshot, or pass a .bscene path)");
			const std::string path = ProjectRelative(reference);
			std::string error;
			Ref<Scene> scene = SceneSerializer::LoadScene(Project::ResolvePath(path), error);
			if (!scene)
				throw CommandError(error);
			return SceneSerializer::SerializeScene(*scene);
		}

		void RegisterScene(CommandRegistry& registry)
		{
			Add(registry, "scene.new", "Replaces the open scene with an empty one (with a camera, sun and sky unless empty=true).", Schema::Object({ { "name", Schema::Nullable(Schema::String("string")) }, { "empty", Schema::Nullable(Schema::Boolean("bool")) } }), [](AutomationSession& session, const json& params) {
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
				return json{ { "entities", scene->GetEntityCount() } }; }, CommandPolicy::EditBoundary);

			Add(registry, "scene.open", "Opens a scene file (project-relative path).", Schema::Object({ { "path", Schema::String("string", true) } }, { "path" }), [](AutomationSession& session, const json& params) {
				const std::string path = ProjectRelative(RequireString(params, "path"));
				std::string error;
				Ref<Scene> scene = SceneSerializer::LoadScene(Project::ResolvePath(path), error);
				if (!scene)
					throw CommandError(error);
				session.SetEditScene(scene, path);
				return json{ { "name", scene->GetName() }, { "entities", scene->GetEntityCount() } }; }, CommandPolicy::EditBoundary);

			Add(registry, "scene.save", "Saves the edit scene (to 'path', or the path it was opened from).", Schema::Object({ { "path", Schema::Nullable(Schema::String("string, project-relative, optional")) } }), [](AutomationSession& session, const json& params) {
				std::string path = OptionalString(params, "path", session.GetScenePath());
				if (path.empty())
					throw CommandError("the scene has never been saved; pass 'path' (e.g. 'Assets/Scenes/Main.bscene')");
				path = ProjectRelative(path);
				std::string error;
				if (!SceneSerializer::SaveScene(*session.GetEditScene(), Project::ResolvePath(path), error))
					throw CommandError(error);
				session.OnFileWritten(path);
				session.SetScenePath(path);
				session.ClearDirty();
				return json{ { "path", path } }; }, CommandPolicy::EditBoundary);

			Add(registry, "scene.hash", "Hash of the current scene state (the play copy while playing); equal states give equal hashes, whatever the entity UUIDs.", Schema::Object({ { "expectHash", Schema::String("string: fail unless the hash equals this one (16 lowercase hex digits)", true) } }), [](AutomationSession& session, const json& params) {
				const std::string hash = SceneSerializer::ComputeStateHash(RequireScene(session));
				CheckExpectedHash(params, hash);
				return json{ { "hash", hash } };
			});

			Add(registry, "scene.snapshot", fmt::format("Stores the current scene state (the play copy while playing) under a name for scene.diff. Snapshots live in memory for the session (at most {} names), survive play stop and scene changes, and a name can be reused to replace one.", AutomationSession::MaxSnapshots), Schema::Object({ { "name", Schema::String("string, snapshot name (not ending in .bscene)", true) } }, { "name" }), [](AutomationSession& session, const json& params) {
				const std::string name = RequireString(params, "name");
				std::string error;
				if (!session.CanSetSnapshot(name, error))
					throw CommandError(error);
				json state = SceneSerializer::SerializeScene(RequireScene(session));
				const size_t entities = state["Entities"].size();
				if (!session.SetSnapshot(name, std::move(state), error))
					throw CommandError(error);
				return json{ { "name", name }, { "entities", entities } };
			});

			Add(registry, "scene.diff", "Compares two scene states: 'from' and 'to' each name a snapshot (scene.snapshot) or a scene file (a project-relative path ending in .bscene); 'to' defaults to the current scene (the play copy while playing). Entities are matched by ID. Returns {equal, settings, added, removed, changed, reordered}: settings and each changed entity's changes are [{path, from, to}] (e.g. 'Transform.Translation'; a component name with from null when it was added, to null when removed); reordered is true when the entities both states have appear in a different order (sibling and UI draw order).", Schema::Object({ { "from", Schema::String("string, snapshot name or .bscene path", true) }, { "to", Schema::Nullable(Schema::String("string, snapshot name or .bscene path (default: the current scene)", true)) }, { "tolerance", Schema::Nullable(Schema::Number("number, floats this close count as equal; integers compare exactly (default 0)", 0.0, 1.0e9)) }, { "assertEqual", Schema::Nullable(Schema::Boolean("bool: fail unless the states are equal; the error lists the first differences")) } }, { "from" }), [](AutomationSession& session, const json& params) {
				const json from = LoadSceneState(session, RequireString(params, "from"));
				const std::string toName = OptionalString(params, "to");
				const json to = toName.empty() ? SceneSerializer::SerializeScene(RequireScene(session)) : LoadSceneState(session, toName);
				SceneDiffOptions options;
				options.Tolerance = OptionalNumber(params, "tolerance", 0.0, 0.0, 1.0e9);
				std::string error;
				std::optional<json> diff = DiffScenes(from, to, options, error);
				if (!diff)
					throw CommandError(error);
				if (OptionalBool(params, "assertEqual", false) && !(*diff)["equal"].get<bool>())
					throw CommandError("scenes differ:\n" + DescribeSceneDiff(*diff));
				return *diff;
			});

			Add(registry, "ui.layout",
				"Lays out the current scene's UI components (the play copy while playing) for a viewport of width x height pixels: the canvas size, every visible UI entity's rect [x, y, width, height] in canvas units in draw order (widgets add their kind, values, target, callback and, while playing, state and focus), the host's view that maps the mouse onto the canvas for the widgets (`view`: origin and size in mouse coordinates and whether widgets take pointer and navigation input; headless, mouse positions are canvas units), and the problems that make UI draw or behave differently than authored (an image that does not load and draws its Color, a font that does not load and falls back to the built-in one or has glyphs that draw blank, an entity with several widget components, a widget callback whose target is missing or has no Script).", Schema::Object({ { "width", Schema::Nullable(Schema::Integer("integer, viewport pixels (default 1920)", 1, 65536)) }, { "height", Schema::Nullable(Schema::Integer("integer, viewport pixels (default 1080)", 1, 65536)) } }), [](AutomationSession& session, const json& params) {
					const auto width = static_cast<float>(OptionalInteger(params, "width", 1920, 1, 65536));
					const auto height = static_cast<float>(OptionalInteger(params, "height", 1080, 1, 65536));
					const glm::vec2 canvas = GameUI::GetCanvasSize({ width, height });
					json items = json::array();
					json problems = json::array();
					std::unordered_map<std::string, std::string> fontErrors;
					Scene& scene = RequireScene(session);
					for (const UILayoutItem& item : UILayout::Layout(scene, canvas))
					{
						const UIBox& rect = item.Rect;
						const uint64_t id = item.Owner.GetUUID();
						json entry = { { "id", id }, { "name", item.Owner.GetName() }, { "rect", { rect.Min.x, rect.Min.y, rect.Size.x, rect.Size.y } } };
						const auto report = [&](const std::string& problem) { problems.push_back({ { "id", id }, { "name", item.Owner.GetName() }, { "problem", problem } }); };
						if (const std::optional<UIWidgetInfo> widget = UILayout::GetWidget(item.Owner))
						{
							entry["widget"] = WidgetSummary(scene, item.Owner, *widget);
							if (widget->SeveralComponents)
								report(fmt::format("several widget components: it acts as its {} only", UIWidgetComponentName(widget->Kind)));
							if (const std::string problem = WidgetCallbackProblem(scene, item.Owner, *widget); !problem.empty())
								report(problem);
						}
						items.push_back(std::move(entry));
						if (const auto* image = item.Owner.TryGetComponent<UIImageComponent>(); image && !image->Texture.empty() && !AssetManager::GetTexture(image->Texture))
							report(fmt::format("UIImage texture '{}': {} (draws its Color instead)", image->Texture, AssetManager::GetError(image->Texture)));
						if (const auto* text = item.Owner.TryGetComponent<UITextComponent>(); text && !text->Font.empty())
						{
							auto [error, added] = fontErrors.try_emplace(text->Font);
							if (added)
								error->second = UIFonts::CheckFile(text->Font);
							if (!error->second.empty())
								report(fmt::format("UIText font '{}': {}", text->Font, error->second));
						}
					}
					const GameUI::HostState host = GameUI::GetHostState();
					const glm::vec2 resolution = GameUI::GetResolution();
					const json view = { { "origin", { host.ViewportOrigin.x, host.ViewportOrigin.y } }, { "size", { host.ViewportSize.x, host.ViewportSize.y } }, { "pointerEnabled", host.PointerEnabled }, { "navigationEnabled", host.NavigationEnabled }, { "resolution", { resolution.x, resolution.y } } };
					return json{ { "canvas", { canvas.x, canvas.y } }, { "items", items }, { "view", view }, { "problems", problems } };
				});

			Add(registry, "scene.get", "Returns the whole open scene as JSON (same format as .bscene files).", Schema::Object(), [](AutomationSession& session, const json&) {
				return SceneSerializer::SerializeScene(RequireScene(session));
			});

			Add(registry, "scene.info", "Summary of the open scene: name, path, entity count, state, unsaved changes, undefined physics layers, animation authoring warnings and the physics step (fixedTimestep: the step in use; fixedTimestepOverride: the scene's own step, or null).", Schema::Object(), [](AutomationSession& session, const json&) {
				Scene& scene = RequireScene(session);
				json animationWarnings = json::array();
				for (auto handle : scene.GetRegistry().view<IDComponent>())
				{
					Entity entity(handle, &scene);
					for (const auto& warning : GetAnimationWarnings(entity))
						animationWarnings.push_back({ { "entity", static_cast<uint64_t>(entity.GetUUID()) }, { "name", entity.GetName() }, { "message", warning } });
				}
				const PhysicsSettings& physics = scene.GetPhysicsSettings();
				return json{
					{ "name", scene.GetName() },
					{ "path", session.GetScenePath() },
					{ "entities", scene.GetEntityCount() },
					{ "playing", session.IsPlaying() },
					{ "dirty", session.IsDirty() },
					{ "animationWarnings", animationWarnings },
					{ "unknownPhysicsLayers", UnknownPhysicsLayersJson(scene, ActivePhysicsLayers()) },
					{ "fixedTimestep", scene.GetFixedTimestep() },
					// Null when the scene follows the project's FixedTimestep.
					{ "fixedTimestepOverride", physics.HasFixedTimestepOverride() ? json(physics.FixedTimestep) : json() },
				};
			});

			Add(registry, "scene.settings", "Patches scene-level settings: {Renderer: {...}, Physics: {...}, Name}.", Schema::Object({ { "Renderer", json{ { "type", "object" }, { "description", "object, e.g. {Exposure, Tonemapper, FSR1Enabled, FSR1Sharpness, SSAOEnabled, ShadowDistance}; FSR1Sharpness: " + Fsr1SharpnessRange() } } }, { "Physics", json{ { "type", "object" }, { "description", "object, e.g. {Gravity: [0,-9.81,0], MaxStepsPerFrame: 8, FixedTimestep: 0.01}; FixedTimestep overrides the project's step for this scene, null removes the override" } } }, { "Name", Schema::String("string") } }), [](AutomationSession& session, const json& params) {
				Scene& scene = RequireScene(session);
				json patch = json::object();
				for (const char* key : { "Renderer", "Physics", "Name" })
				{
					if (params.contains(key))
						patch[key] = params[key];
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
				json result{ { "Renderer", saved["Renderer"] }, { "Physics", saved["Physics"] }, { "Name", saved["Name"] } };
				UUID selected = 0;
				ResolvePostProcess(scene, &selected);
				if (selected != 0 && patch.contains("Renderer"))
					for (const char* key : PostProcessOverrideKeys)
						if (patch["Renderer"].contains(key))
						{
							const auto entity = scene.GetEntityByUUID(selected);
							result["warnings"] = json::array({ fmt::format("Renderer post-process settings are overridden by PostProcess on '{}' ({}); edit that component to affect rendering", entity.GetName(), static_cast<uint64_t>(selected)) });
							break;
						}
				return result;
			});
		}

		void RegisterEntity(CommandRegistry& registry)
		{
			Add(registry, "entity.create", "Creates an entity, optionally with a parent and components ({\"Mesh\": {...}, ...}).", Schema::Object({ { "name", Schema::Nullable(Schema::String("string")) }, { "parent", Schema::Nullable(Schema::Entity("entity ID or name")) }, { "components", Schema::Nullable(json{ { "type", "object" }, { "description", "object: component name -> fields; entity references (Joint BodyEntity, ConnectedEntity) take an ID or name" } }) } }), [](AutomationSession& session, const json& params) {
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

			Add(registry, "entity.destroy", "Destroys an entity and its children.", Schema::Object({ { "entity", Schema::Entity("ID or name") } }, { "entity" }), [](AutomationSession& session, const json& params) {
				Entity entity = RequireEntity(session, params);
				const uint64_t id = entity.GetUUID();
				RequireScene(session).DestroyEntity(entity);
				MarkEdited(session);
				return json{ { "destroyed", id } };
			});

			Add(registry, "entity.get", "Returns an entity with all component data.", Schema::Object({ { "entity", Schema::Entity("ID or name") } }, { "entity" }), [](AutomationSession& session, const json& params) {
				return SceneSerializer::SerializeEntity(RequireEntity(session, params));
			});

			Add(registry, "entity.list", "Lists entities (hierarchy order) with their components; optional name substring / component filter.", Schema::Object({ { "nameContains", Schema::Nullable(Schema::String("string")) }, { "component", Schema::Nullable(Schema::String("string")) } }), [](AutomationSession& session, const json& params) {
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

			Add(registry, "entity.rename", "Renames an entity.", Schema::Object({ { "entity", Schema::Entity("ID or name") }, { "name", Schema::String("string", true) } }, { "entity", "name" }), [](AutomationSession& session, const json& params) {
				Entity entity = RequireEntity(session, params);
				entity.GetComponent<TagComponent>().Tag = RequireString(params, "name");
				MarkEdited(session);
				return EntitySummary(entity);
			});

			Add(registry, "entity.set_parent", "Re-parents an entity (parent null = root), keeping its world transform unless keepWorld=false.", Schema::Object({ { "entity", Schema::Entity("ID or name") }, { "parent", Schema::Nullable(Schema::Entity("ID, name or null")) }, { "keepWorld", Schema::Nullable(Schema::Boolean("bool (default true)")) } }, { "entity" }), [](AutomationSession& session, const json& params) {
				Scene& scene = RequireScene(session);
				Entity entity = RequireEntity(session, params);
				Entity parent = params.contains("parent") && !params["parent"].is_null() ? ResolveEntity(scene, params["parent"], "parent") : Entity{};
				if (!scene.SetParent(entity, parent, OptionalBool(params, "keepWorld", true)))
					throw CommandError("cannot parent an entity to itself or to one of its descendants");
				MarkEdited(session);
				return EntitySummary(entity);
			});

			Add(registry, "entity.duplicate", "Duplicates an entity and its children (new IDs).", Schema::Object({ { "entity", Schema::Entity("ID or name") }, { "name", Schema::String("string, optional", true) } }, { "entity" }), [](AutomationSession& session, const json& params) {
				Scene& scene = RequireScene(session);
				Entity copy = scene.DuplicateEntity(RequireEntity(session, params));
				if (params.contains("name"))
					copy.GetComponent<TagComponent>().Tag = RequireString(params, "name");
				MarkEdited(session);
				return EntitySummary(copy);
			});

			Add(registry, "entity.transform", "Returns local and world transforms of an entity.", Schema::Object({ { "entity", Schema::Entity("ID or name") } }, { "entity" }), [](AutomationSession& session, const json& params) {
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
			Add(registry, "component.set", "Adds the component if missing and sets the given fields (others keep their values).", Schema::Object({ { "entity", Schema::Entity("ID or name") }, { "component", Schema::String("string, e.g. 'RigidBody'", true) }, { "data", json{ { "type", "object" }, { "description", "object of fields; entity references (Joint BodyEntity, ConnectedEntity) take an ID or name" } } } }, { "entity", "component", "data" }), [](AutomationSession& session, const json& params) {
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

			Add(registry, "component.get", "Returns one component's fields.", Schema::Object({ { "entity", Schema::Entity("ID or name") }, { "component", Schema::String("string", true) } }, { "entity", "component" }), [](AutomationSession& session, const json& params) {
				Entity entity = RequireEntity(session, params);
				const std::string name = RequireString(params, "component");
				const ComponentInfo& info = RequireComponentInfo(name);
				if (!info.Has(entity))
					throw CommandError("entity '" + entity.GetName() + "' has no " + name + " component");
				return info.Serialize(entity);
			});

			Add(registry, "component.remove", "Removes a component.", Schema::Object({ { "entity", Schema::Entity("ID or name") }, { "component", Schema::String("string", true) } }, { "entity", "component" }), [](AutomationSession& session, const json& params) {
				Entity entity = RequireEntity(session, params);
				const std::string name = RequireString(params, "component");
				const ComponentInfo& info = RequireComponentInfo(name);
				if (info.IsCore)
					throw CommandError("component '" + name + "' cannot be removed");
				if (!info.Has(entity))
					throw CommandError("entity '" + entity.GetName() + "' has no " + name + " component");
				info.Remove(entity);
				MarkEdited(session);
				return json::object();
			});
		}

		void RegisterAssets(CommandRegistry& registry)
		{
			Add(registry, "asset.list", "Lists project asset files, optionally filtered by extensions (e.g. ['.lua']).", Schema::Object({ { "extensions", Schema::Array("array of strings", Schema::String()) } }), [](AutomationSession&, const json& params) {
				RequireProject();
				// The schema has checked the types.
				std::vector<std::string> extensions;
				if (auto it = params.find("extensions"); it != params.end())
					extensions = it->get<std::vector<std::string>>();
				return json{ { "assets", AssetManager::ListAssets(extensions) } };
			});

			Add(registry, "asset.write", "Writes a text asset (script, prefab, scene, animation graph...) to a project-relative path and reloads it. A .banimgraph is validated before replacing the file. A script written while the game runs (Play, not Simulate) is hot reloaded (see 'scripts').", Schema::Object({ { "path", Schema::String("string", true) }, { "content", Schema::String("string") } }, { "path", "content" }), [](AutomationSession& session, const json& params) {
				RequireProject();
				const std::string path = ProjectRelative(RequireString(params, "path"));
				const json& content = Require(params, "content");
				if (!content.is_string())
					throw CommandError("'content' must be a string");
				if (FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(path)) == ".banimgraph")
				{
					std::string error;
					const auto graph = ParseJson(content.get_ref<const std::string&>(), error);
					if (graph.is_discarded() || !AnimationGraph::FromJson(graph, error))
						throw CommandError("invalid animation graph: " + error);
				}
				if (!FileSystem::WriteTextFile(Project::ResolvePath(path), content.get<std::string>()))
					throw CommandError("cannot write '" + path + "'");
				const AssetReloadReport report = session.OnFileWritten(path);
				return json{ { "path", path }, { "bytes", content.get<std::string>().size() }, { "scripts", ScriptReloadJson(report) } };
			});

			Add(registry, "asset.refresh", "Reloads the asset files changed on disk since changes were last reported (by an earlier refresh, by the editor's automatic reload, or since the project was opened in this process): models, textures and animation graphs load again on their next use (graphs restart from their initial state and parameter defaults), and while the game runs (Play, not Simulate) changed scripts are hot reloaded (instances keep their fields; OnReload is called). With 'paths', reloads those files instead, changed or not (a path that does not exist and was not known is an error, and nothing is reloaded then). Files the session wrote itself (asset.write, scene.save, ...) are not reported again. Without 'paths', fails when Assets/ cannot be read.", Schema::Object({ { "paths", Schema::Array("array of project-relative file paths, optional", Schema::String("string", true)) } }), [](AutomationSession& session, const json& params) {
				RequireProject();
				std::vector<FileChange> changes;
				json warnings = json::array();
				if (const auto it = params.find("paths"); it != params.end())
				{
					// Every path is checked before any is recorded: a rejected request changes nothing, so a later refresh
					// still finds the edits it named.
					for (const json& entry : *it)
					{
						const std::string path = ProjectRelative(entry.get<std::string>());
						std::error_code error;
						const std::filesystem::file_status status = std::filesystem::status(Project::ResolvePath(path), error);
						if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory)
							throw CommandError("cannot read '" + path + "': " + error.message());
						if (std::filesystem::is_directory(status))
							throw CommandError("'" + path + "' is a folder; name the files to reload");
						const bool exists = std::filesystem::is_regular_file(status);
						// A path that neither exists nor was known is a typo, not a removal.
						if (!exists && !session.IsKnownFile(path))
							throw CommandError("'" + path + "' does not exist");
						changes.emplace_back(path, exists ? FileChangeType::Modified : FileChangeType::Removed);
					}
					for (const FileChange& change : changes)
						session.AcknowledgeFile(change.Path);
				}
				else
				{
					std::string error;
					std::string warning;
					changes = session.RefreshFileChanges(error, warning);
					if (!error.empty())
						throw CommandError(error);
					if (!warning.empty())
						warnings.push_back(warning);
				}
				const AssetReloadReport report = session.ReloadAssets(changes);
				json changed = json::array();
				for (const FileChange& change : report.Changed)
					changed.push_back({ { "path", change.Path }, { "type", FileChangeTypeName(change.Type) } });
				json result = { { "changed", changed }, { "reloaded", report.Assets }, { "scripts", ScriptReloadJson(report) } };
				if (!warnings.empty())
					result["warnings"] = std::move(warnings);
				return result;
			});

			Add(registry, "asset.preload", "Requests mesh/texture CPU loads on the shared worker pool. Returns a status (pending, ready or failed) and error for each path. Poll by repeating in a persistent serve/mcp/editor session; use wait=true for one-shot CLI calls. wait=true joins these requests, including embedded image dependencies. Reload/refresh retries failed loads. GPU upload happens in the renderer.", Schema::Object({ { "paths", Schema::Array("array of mesh/texture paths, Model.glb#image/N keys or built-in mesh keys", Schema::String("string", true)) }, { "wait", Schema::Boolean("wait for these loads, default false") } }, { "paths" }), [](AutomationSession&, const json& params) {
				const auto& paths = params.at("paths");
				if (paths.size() > 4096)
					throw CommandError("at most 4096 preload paths are allowed");
				std::vector<std::pair<std::string, bool>> requests;
				// Validate the entire batch before creating work.
				for (const auto& value : paths)
				{
					const std::string raw = value.get<std::string>();
					const std::string key = Primitives::IsBuiltinKey(raw) ? raw : ProjectRelative(raw);
					const bool mesh = Primitives::IsBuiltinKey(key) || AssetManager::IsMeshFile(key);
					const auto marker = key.find("#image/");
					if (!mesh && !AssetManager::IsTextureFile(key) && !(marker != std::string::npos && AssetManager::IsMeshFile(key.substr(0, marker))))
						throw CommandError("unsupported preload asset '" + key + "'");
					requests.emplace_back(key, mesh);
				}
				AssetManager::PublishCompleted();
				const bool wait = OptionalBool(params, "wait", false);
				json result = json::array();
				for (const auto& [key, mesh] : requests)
				{
					bool ready = false;
					if (mesh)
						ready = static_cast<bool>(wait ? AssetManager::GetMesh(key) : AssetManager::RequestMesh(key));
					else
						ready = static_cast<bool>(wait ? AssetManager::GetTexture(key) : AssetManager::RequestTexture(key));
					const char* status = "failed";
					if (ready)
						status = "ready";
					else if (AssetManager::IsLoading(key))
						status = "pending";
					result.push_back({ { "path", key }, { "status", status }, { "error", AssetManager::GetError(key) } });
				}
				return result;
			});

			Add(registry, "asset.read", "Reads a text asset.", Schema::Object({ { "path", Schema::String("string", true) } }, { "path" }), [](AutomationSession&, const json& params) {
				const std::string path = ProjectRelative(RequireString(params, "path"));
				const auto text = FileSystem::ReadTextFile(Project::ResolvePath(path));
				if (!text)
					throw CommandError("cannot read '" + path + "'");
				return json{ { "path", path }, { "content", *text } };
			});

			Add(registry, "asset.import_model", "Imports a static model as an entity hierarchy, or a skinned/animated model as one Animator root.", Schema::Object({ { "path", Schema::String("string, .gltf/.glb or builtin://Cube", true) }, { "parent", Schema::Nullable(Schema::Entity("ID or name, optional")) } }, { "path" }), [](AutomationSession& session, const json& params) {
				Scene& scene = RequireScene(session);
				Entity parent = params.contains("parent") && !params["parent"].is_null() ? ResolveEntity(scene, params["parent"], "parent") : Entity{};
				std::string error;
				Entity root = AssetManager::ImportModel(scene, RequireString(params, "path"), parent, error);
				if (!root)
					throw CommandError(error);
				MarkEdited(session);
				return EntitySummary(root);
			});

			Add(registry, "prefab.create", "Saves an entity tree as a prefab file.", Schema::Object({ { "entity", Schema::Entity("ID or name") }, { "path", Schema::String("string, e.g. 'Assets/Prefabs/Enemy.bprefab'", true) } }, { "entity", "path" }), [](AutomationSession& session, const json& params) {
				Entity entity = RequireEntity(session, params);
				const std::string path = ProjectRelative(RequireString(params, "path"));
				std::string error;
				if (!SceneSerializer::SavePrefab(entity, Project::ResolvePath(path), error))
					throw CommandError(error);
				session.OnFileWritten(path);
				return json{ { "path", path } };
			});

			Add(registry, "prefab.instantiate", "Instantiates a prefab into the scene.", Schema::Object({ { "path", Schema::String("string", true) }, { "parent", Schema::Nullable(Schema::Entity("ID or name, optional")) }, { "position", Schema::Vector("[x, y, z], optional", 3) } }, { "path" }), [](AutomationSession& session, const json& params) {
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

			Add(registry, "script.check", "Compiles a Lua script in isolation and returns its Properties, or the error.", Schema::Object({ { "path", Schema::String("string", true) } }, { "path" }), [](AutomationSession&, const json& params) {
				const std::string path = ProjectRelative(RequireString(params, "path"));
				std::string error;
				const auto properties = ScriptEngine::LoadScriptProperties(path, error);
				if (!properties)
					throw CommandError(error);
				return json{ { "properties", *properties } };
			});
		}

		// The comparison parameters image.compare and render.compare share.
		json ImageCompareProperties()
		{
			return {
				{ "diff", Schema::Nullable(Schema::String("string, output .png, optional")) },
				{ "pixelThreshold", Schema::Nullable(Schema::Integer("integer 0-255, default 8", 0, 255)) },
				{ "maxPercent", Schema::Nullable(Schema::Number("number, default 0.5", 0.0, 100.0)) },
				{ "assertMatch", Schema::Nullable(Schema::Boolean("bool, default false")) },
			};
		}

		// Whether two paths name the same file, whether or not it exists yet (symlinks and '..' resolved).
		bool SameFile(const std::filesystem::path& a, const std::filesystem::path& b)
		{
			std::error_code error;
			const std::filesystem::path first = std::filesystem::weakly_canonical(a, error);
			if (error)
				return a.lexically_normal() == b.lexically_normal();
			const std::filesystem::path second = std::filesystem::weakly_canonical(b, error);
			return error ? a.lexically_normal() == b.lexically_normal() : first == second;
		}

		// The wording is stable: the golden-image ctests skip (not fail) on a missing reference.
		void RequireReferenceImage(const std::filesystem::path& referencePath)
		{
			if (!std::filesystem::exists(referencePath))
				throw CommandError("reference image not found: '" + referencePath.string() + "'");
		}

		json CompareImageFiles(const std::filesystem::path& actualPath, const std::filesystem::path& referencePath, const json& params)
		{
			ImageCompareOptions options;
			options.PixelThreshold = static_cast<uint8_t>(OptionalInteger(params, "pixelThreshold", options.PixelThreshold, 0, 255));
			options.MaxDifferingPercent = OptionalNumber(params, "maxPercent", options.MaxDifferingPercent, 0.0, 100.0);
			const std::string diffParam = OptionalString(params, "diff");
			const std::filesystem::path diffPath = diffParam.empty() ? std::filesystem::path() : Project::ResolvePath(diffParam);

			// The diff must never replace (or, when the sizes differ, delete) one of the compared images.
			if (!diffPath.empty() && (SameFile(diffPath, actualPath) || SameFile(diffPath, referencePath)))
				throw CommandError("'diff' must not be one of the compared images");
			std::string error;
			RequireReferenceImage(referencePath);
			Ref<TextureSource> reference = TextureSource::LoadFromFile(referencePath, error);
			if (!reference)
				throw CommandError("cannot load reference image: " + error);
			Ref<TextureSource> actual = TextureSource::LoadFromFile(actualPath, error);
			if (!actual)
				throw CommandError("cannot load image: " + error);

			ImageCompareResult result = CompareImages(*actual, *reference, options, !diffPath.empty());
			json diff = nullptr;
			if (!diffPath.empty())
			{
				if (result.SizeMatches)
				{
					if (!WritePng(diffPath, result.DiffImage, result.Width, result.Height, error))
						throw CommandError(error);
					diff = FileSystem::PathToUtf8(diffPath.lexically_normal());
				}
				else
				{
					// No diff of images of different sizes; an older one must not pass for this comparison's (only a file).
					std::error_code removeError;
					if (std::filesystem::is_regular_file(diffPath, removeError))
						std::filesystem::remove(diffPath, removeError);
					if (removeError)
						BS_CORE_WARN("image.compare: cannot remove the old diff image '{}': {}", FileSystem::PathToUtf8(diffPath), removeError.message());
				}
			}

			json response = {
				{ "matches", result.Matches },
				{ "sizeMatches", result.SizeMatches },
				{ "size", { actual->Width, actual->Height } },
				{ "referenceSize", { reference->Width, reference->Height } },
				{ "maxDelta", result.MaxDelta },
				{ "meanDelta", result.MeanDelta },
				{ "differingPercent", result.DifferingPercent },
				{ "similarity", result.SizeMatches ? 1.0 - result.MeanDelta / 255.0 : 0.0 },
				{ "actual", FileSystem::PathToUtf8(actualPath.lexically_normal()) },
				{ "diff", diff },
			};
			if (OptionalBool(params, "assertMatch", false) && !result.Matches)
				throw CommandError("images differ: " + response.dump(-1, ' ', false, json::error_handler_t::replace));
			return response;
		}

		void RegisterPlay(CommandRegistry& registry)
		{
			Add(registry, "play.start", "Starts play mode on a copy of the scene (scripts, physics, audio run).", Schema::Object(), [](AutomationSession& session, const json&) {
				std::string error;
				if (!session.StartPlay(error))
					throw CommandError(error);
				return json{ { "playing", true } }; }, CommandPolicy::EditBoundary);

			Add(registry, "play.stop", "Stops play mode and restores the edit scene.", Schema::Object(), [](AutomationSession& session, const json&) {
				session.StopPlay();
				return json{ { "playing", false } };
			});

			Add(registry, "play.step", "Advances play mode by N fixed frames (default dt 1/60) and reports script errors and the playing scene (which Lua Scene.Load may switch).", Schema::Object({ { "frames", Schema::Nullable(Schema::Integer("integer (default 1)", 1, 100000)) }, { "dt", json{ { "type", "number" }, { "description", "number, seconds per frame" }, { "exclusiveMinimum", 0.0 }, { "maximum", 0.25 } } }, { "assertNoErrors", Schema::Nullable(Schema::Boolean("bool: fail the command if any script error occurred")) }, { "hash", Schema::Nullable(Schema::Boolean("bool: also return stateHash (see scene.hash)")) }, { "expectHash", Schema::String("string: fail unless the state hash after the step (which has run either way) equals this one; also returns stateHash", true) } }), [](AutomationSession& session, const json& params) {
				if (!session.IsPlaying())
					throw CommandError("not playing (use play.start)");
				const uint32_t frames = static_cast<uint32_t>(OptionalInteger(params, "frames", 1, 1, 100000));
				float dt = 1.0f / 60.0f;
				if (auto it = params.find("dt"); it != params.end())
				{
					// A positive JSON double can still underflow to zero in the float timestep.
					if (!JsonToFloat(*it, dt) || dt <= 0.0f || dt > 0.25f)
						throw CommandError("'dt' must be a number in (0, 0.25]");
				}
				session.Step(frames, dt);
				Scene& scene = RequireScene(session);
				json errors = scene.GetScriptEngine() ? json(scene.GetScriptEngine()->GetErrors()) : json::array();
				if (OptionalBool(params, "assertNoErrors", false) && !errors.empty())
					throw CommandError("script errors: " + errors.dump(-1, ' ', false, json::error_handler_t::replace));
				json result = { { "time", scene.GetTime() }, { "frame", scene.GetFrameCount() }, { "scriptErrors", errors }, { "quitRequested", scene.IsQuitRequested() }, { "scene", scene.GetName() }, { "scenePath", scene.GetPath() } };
				if (OptionalBool(params, "hash", false) || params.contains("expectHash"))
				{
					const std::string hash = SceneSerializer::ComputeStateHash(scene);
					CheckExpectedHash(params, hash);
					result["stateHash"] = hash;
				}
				return result;
			});

			Add(registry, "replay.record_start", "Starts recording input.key/input.mouse/input.gamepad commands with the play frame they arrive on. The mouse position and the injected gamepads (with what they hold) are recorded as they are on that frame, and the host's view whenever it changes, checked before every frame and every recorded command: where mouse positions land on the UI canvas, the game resolution scripts read the mouse in, and whether the game has the pointer and the keyboard (routing gates every game input), so replays run the same way in any host. A recording begun before play marks where play started (a playStart event): what came before it applies before the replay starts play, so OnCreate sees the same view and input. Physical gamepads are not recorded (replays run without them): they are listed in physicalGamepads.", Schema::Object(), [](AutomationSession& session, const json&) {
				session.StartRecording();
				json physical = json::array();
				for (uint32_t index = 0; index < Input::MaxGamepads; index++)
				{
					if (Input::GetGamepadSource(index) == GamepadSource::Device)
						physical.push_back(index);
				}
				if (!physical.empty())
					BS_CORE_WARN("replay.record_start: physical gamepads {} are connected; replays run without them, so their input will not replay", physical.dump(-1, ' ', false, json::error_handler_t::replace));
				return json{ { "recording", true }, { "physicalGamepads", physical } };
			});

			Add(registry, "replay.record_stop", "Stops recording and writes the replay file (project-relative path, e.g. 'Replays/run.breplay').", Schema::Object({ { "path", Schema::String("string", true) } }, { "path" }), [](AutomationSession& session, const json& params) {
				RequireProject();
				const std::string path = ProjectRelative(RequireString(params, "path"));
				if (!session.IsRecording())
					throw CommandError("not recording (use replay.record_start)");
				Scene* playing = session.IsPlaying() ? &RequireScene(session) : nullptr;
				const uint64_t frames = playing ? playing->GetFrameCount() : 0;
				// The seed is part of the run (Lua randomness and particles follow it): the seed of the session
				// recorded, which project.set randomSeed during or after the recording does not change. A recording
				// that saw no play session replays with the project's seed.
				const uint32_t seed = session.GetRecordingSeed().value_or(Project::GetActiveRandomSeed());
				const bool reloaded = session.HasReloadedScriptsWhileRecording();
				const json events = session.StopRecording();
				json replay = { { "Format", "BasaltReplay" }, { "Version", 1 }, { "Frames", frames }, { "RandomSeed", seed }, { "Events", events } };
				// The recorded run's final state: replay.run with assertHash checks a replay reproduces it.
				if (playing)
					replay["StateHash"] = SceneSerializer::ComputeStateHash(*playing);
				if (!FileSystem::WriteTextFile(Project::ResolvePath(path), replay.dump(1, '\t')))
					throw CommandError("cannot write '" + path + "'");
				session.OnFileWritten(path);
				json result = { { "path", path }, { "events", events.size() }, { "frames", frames } };
				if (reloaded)
					result["warnings"] = json::array({ "scripts were hot reloaded during the recording: replaying runs the final code from the start, so it may not reproduce the run" });
				return result;
			});

			Add(registry, "replay.run", "Restarts play from the edit scene with no input held, re-applies a replay's inputs (and recorded host views) on their frames without physical gamepads, and returns the state hash. Play keeps running afterwards for inspection; the host view and gamepads are restored.", Schema::Object({ { "path", Schema::String("string, replay file", true) }, { "frames", Schema::Nullable(Schema::Integer("integer, default: the recorded length", 0, 10000000)) }, { "assertNoErrors", Schema::Nullable(Schema::Boolean("bool")) }, { "assertHash", Schema::Nullable(Schema::Boolean("bool: fail unless the final state hash equals the recorded one")) } }, { "path" }), [](AutomationSession& session, const json& params) {
				RequireProject();
				const std::string path = ProjectRelative(RequireString(params, "path"));
				const auto text = FileSystem::ReadTextFile(Project::ResolvePath(path));
				if (!text)
					throw CommandError("cannot read '" + path + "'");
				const json replay = ParseJson(*text);
				if (!replay.is_object() || replay.value("Format", "") != "BasaltReplay" || !replay.contains("Events") || !replay["Events"].is_array())
					throw CommandError("'" + path + "' is not a Basalt replay file");
				// Replays from before seeds were stored ran with Random::DefaultSeed, the only seed there was.
				const auto storedSeed = replay.find("RandomSeed");
				uint32_t recordedSeed = Random::DefaultSeed;
				if (storedSeed != replay.end() && !JsonToUInt32(*storedSeed, recordedSeed))
					throw CommandError("'" + path + "' has an invalid RandomSeed (must be an integer between 0 and " + std::to_string(std::numeric_limits<uint32_t>::max()) + ")");
				// The replay restarts play, which seeds from the project.
				if (const uint32_t projectSeed = Project::GetActiveRandomSeed(); recordedSeed != projectSeed)
				{
					const std::string recordedWith = storedSeed != replay.end() ? "was recorded with random seed " + std::to_string(recordedSeed) : "has no RandomSeed (it was recorded before seeds were stored, with the default seed " + std::to_string(recordedSeed) + ")";
					throw CommandError("'" + path + "' " + recordedWith + " but the project uses " + std::to_string(projectSeed) + " (project.set randomSeed)");
				}
				const int64_t recorded = replay.value("Frames", int64_t{ 0 });
				const uint32_t frames = static_cast<uint32_t>(OptionalInteger(params, "frames", recorded, 0, 10000000));

				session.StopPlay();
				// A replay starts from no input (whether or not play was running): no keys or buttons held, no mouse
				// position, no injected gamepads. It runs with the view it recorded ("view" events; the headless view
				// for recordings without one) whatever the host, and with only the gamepads it injects, so it plays
				// the same in the editor and headless.
				Input::Reset();
				Input::ResetMousePosition();
				Input::ForgetInjectedGamepads();
				// Constructed explicitly: MSVC cannot aggregate-initialize a local class inside a lambda (C2326).
				struct RestoreHost
				{
					explicit RestoreHost(AutomationSession& session)
						: Session(session)
						, Host(GameUI::GetHostState())
						, DeviceGamepads(Input::IsDeviceGamepadsEnabled())
						// The host would give the game its own view as play starts.
						, BeforeRuntimeStart(std::exchange(session.BeforeRuntimeStart, nullptr))
					{
					}
					RestoreHost(const RestoreHost&) = delete;
					RestoreHost& operator=(const RestoreHost&) = delete;
					AutomationSession& Session;
					GameUI::HostState Host;
					bool DeviceGamepads;
					std::function<void()> BeforeRuntimeStart;
					~RestoreHost()
					{
						GameUI::SetHostState(Host);
						Input::SetDeviceGamepadsEnabled(DeviceGamepads);
						Session.BeforeRuntimeStart = std::move(BeforeRuntimeStart);
					}
				} restore(session);
				GameUI::SetHostState({});
				Input::SetDeviceGamepadsEnabled(false);
				const json& events = replay["Events"];
				// Events recorded at frame N were applied before the update that produced frame N + 1. A recording that
				// began before play has a "playStart" marker: what came before it (the view play started with, keys
				// already held) applies before the replay starts play, so the scripts' OnCreate sees it too. Recordings
				// without one apply their leading frame-0 views then.
				std::optional<size_t> playStart;
				for (size_t i = 0; i < events.size(); i++)
				{
					const json& event = events[i];
					const auto frame = event.is_object() ? event.find("frame") : event.end();
					uint32_t frameNumber = 0;
					if (!event.is_object() || (frame != event.end() && !JsonToUInt32(*frame, frameNumber)) || !event.value("command", json()).is_string() || (event.contains("params") && !event["params"].is_object()))
						throw CommandError(fmt::format("'{}' event {} is not {{frame: integer, command: string, params: object}}", path, i));
					if (!playStart && event["command"] == "playStart")
						playStart = i;
				}
				const auto applyEvent = [&](size_t index) {
					const json& event = events[index];
					const std::string command = event["command"].get<std::string>();
					const json eventParams = event.value("params", json::object());
					try
					{
						if (command == "input.key")
							ApplyKeyInput(eventParams);
						else if (command == "input.mouse")
							ApplyMouseInput(eventParams);
						else if (command == "input.gamepad")
							ApplyGamepadInput(eventParams);
						else if (command == "view")
							ApplyViewEvent(eventParams);
						else if (command != "playStart")
							throw CommandError("unknown command '" + command + "'");
					}
					catch (const CommandError& e)
					{
						throw CommandError(fmt::format("'{}' event {} (frame {}, {}): {}", path, index, event.value("frame", uint64_t{ 0 }), command, e.what()));
					}
				};
				const auto beforePlay = [&](size_t index) {
					if (playStart)
						return index <= *playStart;
					return events[index].value("frame", uint64_t{ 0 }) == 0 && events[index]["command"] == "view";
				};
				size_t next = 0;
				for (; next < events.size() && beforePlay(next); next++)
					applyEvent(next);
				std::string error;
				if (!session.StartPlay(error))
					throw CommandError(error);
				for (uint32_t frame = 0; frame <= frames; frame++)
				{
					for (; next < events.size() && events[next].value("frame", uint64_t{ 0 }) <= frame; next++)
						applyEvent(next);
					if (frame < frames)
						session.Step(1, 1.0f / 60.0f);
				}

				Scene& scene = RequireScene(session);
				json errors = scene.GetScriptEngine() ? json(scene.GetScriptEngine()->GetErrors()) : json::array();
				if (OptionalBool(params, "assertNoErrors", false) && !errors.empty())
					throw CommandError("script errors: " + errors.dump(-1, ' ', false, json::error_handler_t::replace));
				const std::string hash = SceneSerializer::ComputeStateHash(scene);
				const std::string recordedHash = replay.value("StateHash", "");
				if (OptionalBool(params, "assertHash", false))
				{
					if (recordedHash.empty() || frames != static_cast<uint32_t>(recorded))
						throw CommandError("assertHash needs the full recorded length of a replay that stored a StateHash");
					if (hash != recordedHash)
						throw CommandError("replay diverged: state hash " + hash + ", recorded " + recordedHash);
				}
				return json{ { "frame", scene.GetFrameCount() }, { "stateHash", hash }, { "recordedHash", recordedHash }, { "scriptErrors", errors } }; }, CommandPolicy::EditBoundary);

			Add(registry, "input.key", "Sets a key's state for play mode (e.g. {key: 'Space', down: true}). Recorded while replay recording is on.", Schema::Object({ { "key", Schema::String("string key name", true) }, { "down", Schema::Nullable(Schema::Boolean("bool")) } }, { "key" }), [](AutomationSession& session, const json& params) {
				ApplyKeyInput(params);
				session.RecordInput("input.key", params);
				return json::object();
			});

			Add(registry, "input.mouse", "Sets mouse position and/or a button state for play mode. Recorded while replay recording is on.", Schema::Object({ { "position", Schema::Vector("[x, y]", 2) }, { "button", Schema::String("string (Left, Right, Middle)", true) }, { "down", Schema::Nullable(Schema::Boolean("bool")) } }), [](AutomationSession& session, const json& params) {
				ApplyMouseInput(params);
				session.RecordInput("input.mouse", params);
				return json::object();
			});

			Add(registry, "input.gamepad",
				"Connects, disconnects or drives an injected gamepad for play mode (e.g. {index: 0, connected: true}, {button: 'A', down: true}, {axis: 'LeftX', value: -1}). A device poll never overrides an injected gamepad. Recorded while replay recording is on.", Schema::Object({ { "index", Schema::Nullable(Schema::Integer(fmt::format("integer 0-{} (default 0)", Input::MaxGamepads - 1), 0, Input::MaxGamepads - 1)) }, { "connected", Schema::Boolean("bool") }, { "name", Schema::String("string, the gamepad's name when connecting", true) }, { "button", Schema::String("string (A, B, X, Y, LeftBumper, RightBumper, Back, Start, Guide, LeftThumb, RightThumb, DPadUp, DPadRight, DPadDown, DPadLeft)", true) }, { "down", Schema::Nullable(Schema::Boolean("bool (default true)")) }, { "axis", Schema::String("string (LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger)", true) }, { "value", Schema::Number("number: sticks -1..1 (+Y down), triggers 0..1", -1.0, 1.0) } }),
				[](AutomationSession& session, const json& params) {
					ApplyGamepadInput(params);
					session.RecordInput("input.gamepad", params);
					return json::object();
				});

			Add(registry, "input.actions", "Returns {playing, actions}. While playing: the session's actions (with the bindings scripts gave them) and each one's Value (a button's strength 0..1, an axis -1..1), Down, Pressed and Released as the game sees them this frame. Otherwise (needs a project): the project's actions.", Schema::Object(), [](AutomationSession& session, const json&) {
				const Scene* scene = session.GetScene().get();
				const bool playing = session.IsPlaying() && scene && scene->GetState() == SceneState::Play;
				if (!playing)
					RequireProject();
				const InputActionMap& actions = playing ? scene->GetInputActions() : Project::GetActiveConfig()->InputActions;
				json result = actions.ActionsToJson();
				if (playing)
				{
					for (size_t i = 0; i < actions.GetActions().size(); i++)
					{
						const InputAction& action = actions.GetActions()[i];
						result[i]["Value"] = InputActionMap::GetValue(action);
						result[i]["Down"] = InputActionMap::IsDown(action);
						result[i]["Pressed"] = InputActionMap::IsPressed(action);
						result[i]["Released"] = InputActionMap::IsReleased(action);
					}
				}
				return json{ { "playing", playing }, { "actions", result } };
			});

			Add(registry, "lua.exec", "Runs Lua in the playing scene's script state; returns the value of the expression.", Schema::Object({ { "code", Schema::String("string", true) } }, { "code" }), [](AutomationSession& session, const json& params) {
				Scene& scene = RequireScene(session);
				if (!session.IsPlaying() || !scene.GetScriptEngine())
					throw CommandError("not playing (use play.start)");
				std::string result;
				if (!scene.GetScriptEngine()->ExecuteString(RequireString(params, "code"), result))
					throw CommandError(result);
				return json{ { "result", result } };
			});

			Add(registry, "render.screenshot", "Renders the current scene from its primary camera (the editor camera when the scene has none), with its game UI, to a PNG (editor / GPU hosts only). The editor's debug view applies; editor.game_view's path never uses one and fails without a primary camera.", Schema::Object({ { "path", Schema::String("string, output .png", true) }, { "width", Schema::Nullable(Schema::Integer("integer", 16, 8192)) }, { "height", Schema::Nullable(Schema::Integer("integer", 16, 8192)) } }, { "path" }), [](AutomationSession& session, const json& params) {
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

			Add(registry, "memory.stats", "Returns logical retained CPU asset payload bytes, requested GPU buffer bytes, and live Lua heap bytes, with process-lifetime peaks and live allocation counts. Not RSS or driver residency; excludes allocator overhead, GPU textures and render targets. Available headlessly.", Schema::Object(), [](AutomationSession&, const json&) {
				return MemoryTracker::ToJson();
			});

			Add(registry, "render.stats", "Returns the scene renderer's statistics: viewport and render size, render scale and dynamic resolution state, draw and culling counts, and the GPU time of each pass averaged over recent frames (editor / GPU hosts only).", Schema::Object(), [](AutomationSession& session, const json&) {
				if (!session.RenderStats)
					throw CommandError("render statistics need a GPU host (the editor); this host is '" + session.HostName + "'");
				json stats;
				std::string error;
				if (!session.RenderStats(stats, error))
					throw CommandError(error);
				stats["memory"] = MemoryTracker::ToJson();
				return stats;
			});

			// Headless, so tests and agents can check renders from any host (golden-image tests use it).
			const json compareProperties = ImageCompareProperties();
			json compareSchema = Schema::Object({ { "actual", Schema::String("string, image path (relative paths are project-relative)", true) }, { "reference", Schema::String("string, image path", true) } }, { "actual", "reference" });
			compareSchema["properties"].update(compareProperties);
			Add(registry, "image.compare", "Compares a PNG with a reference image within a tolerance; optionally writes a diff image (not when the sizes differ: 'diff' is then null) and fails on mismatch. similarity is 1 - meanDelta / 255 (1 = identical, 0 when the sizes differ); actual and diff are the resolved paths.", std::move(compareSchema),
				[](AutomationSession&, const json& params) {
					return CompareImageFiles(Project::ResolvePath(RequireString(params, "actual")), Project::ResolvePath(RequireString(params, "reference")), params);
				});

			json renderCompareSchema = Schema::Object({ { "reference", Schema::String("string, reference image path (relative paths are project-relative)", true) }, { "output", Schema::Nullable(Schema::String("string, where the render is written (default Cache/RenderCompare.png; relative paths are project-relative)", true)) }, { "width", Schema::Nullable(Schema::Integer("integer (default 1280)", 16, 8192)) }, { "height", Schema::Nullable(Schema::Integer("integer (default 720)", 16, 8192)) } }, { "reference" });
			renderCompareSchema["properties"].update(compareProperties);
			Add(registry, "render.compare", "Renders the current scene like render.screenshot and compares the image with a reference like image.compare (editor / GPU hosts only). Returns the image.compare result; 'actual' is where the render was written.", std::move(renderCompareSchema), [](AutomationSession& session, const json& params) {
				if (!session.Screenshot)
					throw CommandError("render.compare needs a GPU host (the editor); this host is '" + session.HostName + "'. Headless, compare a 'BasaltRuntime --frames N --screenshot' image with image.compare");
				const std::filesystem::path outputPath = Project::ResolvePath(OptionalString(params, "output", "Cache/RenderCompare.png"));
				const uint32_t width = static_cast<uint32_t>(OptionalInteger(params, "width", 1280, 16, 8192));
				const uint32_t height = static_cast<uint32_t>(OptionalInteger(params, "height", 720, 16, 8192));
				const std::filesystem::path referencePath = Project::ResolvePath(RequireString(params, "reference"));
				// Before rendering, so a missing reference costs no render. The screenshot creates the output folder.
				RequireReferenceImage(referencePath);
				if (SameFile(outputPath, referencePath))
					throw CommandError("'output' is the reference image; the render would replace it (pass another 'output')");
				std::string error;
				if (!session.Screenshot(outputPath, width, height, error))
					throw CommandError(error);
				return CompareImageFiles(outputPath, referencePath, params);
			});
		}

		// --- Assertions ----------------------------------------------------------------------------
		// Checks that read like a game test's specification. They fail with "assertion failed: ..." so a batch
		// can expect the failure (expectError) and a log shows what was wrong.

		// Floats in assert.component fields and events.poll patterns match within this unless 'tolerance' says otherwise.
		constexpr double DefaultMatchTolerance = 1.0e-4;
		constexpr double MaxMatchTolerance = 1.0e9;

		// The 'tolerance' parameter of a matching command, validated (never negative or NaN: JsonNearlyEqual asserts on it).
		double MatchTolerance(const json& params)
		{
			return OptionalNumber(params, "tolerance", DefaultMatchTolerance, 0.0, MaxMatchTolerance);
		}

		std::string EntityLabel(Entity entity)
		{
			return fmt::format("'{}' (ID {})", entity.GetName(), static_cast<uint64_t>(entity.GetUUID()));
		}

		// Runs a lookup whose failure means the scene does not match the assertion: the error gets its prefix.
		template<typename F>
		auto Asserting(F&& lookup)
		{
			try
			{
				return lookup();
			}
			catch (const CommandError& e)
			{
				throw CommandError(std::string("assertion failed: ") + e.what());
			}
		}

		void RegisterAssertions(CommandRegistry& registry)
		{
			Add(registry, "assert.entity_exists", "Fails unless the current scene (the play copy while playing) has the entity: by ID, or by name (any number of entities with that name). With exists=false, fails if it does exist; with count, unless exactly that many entities have the name (or ID). Returns {count, ids}.", Schema::Object({ { "entity", Schema::Entity("ID or name") }, { "exists", Schema::Nullable(Schema::Boolean("bool (default true)")) }, { "count", Schema::Nullable(Schema::Integer("integer, exact number of matching entities", 0, 1000000)) } }, { "entity" }), [](AutomationSession& session, const json& params) {
				if (HasParam(params, "count") && HasParam(params, "exists"))
					throw CommandError("pass either 'exists' or 'count', not both");
				std::string what;
				const std::vector<Entity> matches = FindEntities(RequireScene(session), Require(params, "entity"), "entity", what);
				if (HasParam(params, "count"))
				{
					const auto expected = static_cast<size_t>(OptionalInteger(params, "count", 0, 0, 1000000));
					if (matches.size() != expected)
						throw CommandError(fmt::format("assertion failed: {} {} {}, expected {}", matches.size(), matches.size() == 1 ? "entity" : "entities", what, expected));
				}
				else if (OptionalBool(params, "exists", true))
				{
					if (matches.empty())
						throw CommandError("assertion failed: no entity " + what);
				}
				else if (!matches.empty())
					throw CommandError("assertion failed: an entity " + what + " exists");
				json ids = json::array();
				for (Entity entity : matches)
					ids.push_back(static_cast<uint64_t>(entity.GetUUID()));
				return json{ { "count", matches.size() }, { "ids", ids } };
			});

			Add(registry, "assert.component", "Fails unless the entity (which must exist, with a unique name) has the component and, for each given field, the value component.get returns: floats within 'tolerance', integers and everything else exactly, vectors element-wise, objects may have more keys than given. With exists=false, fails if the entity has the component. Returns the component's fields. The error lists every mismatch.", Schema::Object({ { "entity", Schema::Entity("ID or name") }, { "component", Schema::String("string, e.g. 'Transform'", true) }, { "fields", json{ { "type", "object" }, { "description", "object of expected field values (component.get format; entity references take an ID or name)" } } }, { "tolerance", Schema::Nullable(Schema::Number("number (default 0.0001)", 0.0, 1.0e9)) }, { "exists", Schema::Nullable(Schema::Boolean("bool (default true)")) } }, { "entity", "component" }), [](AutomationSession& session, const json& params) {
				Scene& scene = RequireScene(session);
				Entity entity = Asserting([&]() { return ResolveEntity(scene, Require(params, "entity")); });
				const std::string name = RequireString(params, "component");
				const ComponentInfo& info = RequireComponentInfo(name);
				const bool hasFields = params.contains("fields");
				if (!OptionalBool(params, "exists", true))
				{
					if (hasFields)
						throw CommandError("'fields' cannot be checked with exists=false");
					if (info.Has(entity))
						throw CommandError("assertion failed: " + EntityLabel(entity) + " has a " + name + " component");
					return json::object();
				}
				if (!info.Has(entity))
					throw CommandError("assertion failed: " + EntityLabel(entity) + " has no " + name + " component");
				json data = info.Serialize(entity);
				if (!hasFields)
					return data;
				for (const auto& [field, value] : params["fields"].items())
				{
					if (std::ranges::find(info.Fields, field) == info.Fields.end())
						throw CommandError(fmt::format("unknown field '{}' for {} (valid: {})", field, name, fmt::join(info.Fields, ", ")));
				}
				const json expected = Asserting([&]() { return ResolveEntityFields(scene, name, params["fields"]); });
				std::vector<std::string> mismatches;
				MatchJsonSubset(data, expected, MatchTolerance(params), name, mismatches);
				if (!mismatches.empty())
					throw CommandError(fmt::format("assertion failed: {} {}", EntityLabel(entity), fmt::join(mismatches, "; ")));
				return data;
			});
		}

		// --- Events --------------------------------------------------------------------------------

		// The required types (TypeBits) none of the events has, by name.
		std::vector<std::string> MissingEventTypes(std::span<const json> events, uint32_t requiredTypes)
		{
			std::vector<std::string> missing;
			for (uint32_t index = 0; index < static_cast<uint32_t>(AutomationEventType::Count); index++)
			{
				const auto type = static_cast<AutomationEventType>(index);
				const char* name = AutomationEventTypeName(type);
				if ((requiredTypes & AutomationEventHub::TypeBit(type)) && std::ranges::none_of(events, [name](const json& event) { return event.value("event", "") == name; }))
					missing.emplace_back(name);
			}
			return missing;
		}

		// TypeBits of an array of event type names (validated by the schema's enum).
		uint32_t ParseEventTypes(const json& names)
		{
			uint32_t types = 0;
			for (const json& name : names)
			{
				const std::optional<AutomationEventType> type = AutomationEventTypeFromName(name.get<std::string>());
				if (!type)
					throw CommandError("unknown event type " + name.dump());
				types |= AutomationEventHub::TypeBit(*type);
			}
			return types;
		}

		// "2 playState, 1 log", or "none".
		std::string DescribeEventCounts(std::span<const json> events)
		{
			std::map<std::string, size_t> counts;
			for (const json& event : events)
				counts[event.value("event", "")]++;
			if (counts.empty())
				return "none";
			std::vector<std::string> kinds;
			kinds.reserve(counts.size());
			for (const auto& [kind, count] : counts)
				kinds.push_back(fmt::format("{} {}", count, kind));
			return fmt::format("{}", fmt::join(kinds, ", "));
		}

		// Whether the events include every required type (TypeBits): MissingEventTypes without building names, for the poll
		// check that runs under the hub's lock.
		bool HasEveryEventType(std::span<const json> events, uint32_t requiredTypes)
		{
			uint32_t present = 0;
			for (const json& event : events)
			{
				if (const std::optional<AutomationEventType> type = AutomationEventTypeFromName(event.value("event", "")))
					present |= AutomationEventHub::TypeBit(*type);
			}
			return (present & requiredTypes) == requiredTypes;
		}

		// Patterns one events.poll may list. They are matched against up to MaxQueuedEvents events while the hub is locked,
		// so matching an event allocates nothing and never logs.
		constexpr size_t MaxEventPatterns = 64;

		// An events.poll 'expect' pattern. A contact names its bodies in UUID order, which a test cannot know, so a pattern
		// that names a or b also matches the contact seen from the other body: Flipped is the pattern with a and b swapped
		// and its normal (which points from b toward a, like ContactInfo::Flipped's) reversed. Flipping the pattern once,
		// before the poll, keeps both orientations to a plain subset match. A pattern naming neither body keeps the
		// published orientation.
		struct EventPattern
		{
			const json* Pattern = nullptr;
			std::optional<json> Flipped;
		};

		std::vector<EventPattern> MakeEventPatterns(const json& patterns)
		{
			std::vector<EventPattern> result;
			result.reserve(patterns.size());
			for (const json& pattern : patterns)
			{
				EventPattern& entry = result.emplace_back();
				entry.Pattern = &pattern;
				if (!pattern.contains("a") && !pattern.contains("b"))
					continue;
				json flipped = pattern;
				flipped.erase("a");
				flipped.erase("b");
				if (pattern.contains("a"))
					flipped["b"] = pattern["a"];
				if (pattern.contains("b"))
					flipped["a"] = pattern["b"];
				if (auto normal = flipped.find("normal"); normal != flipped.end() && normal->is_array())
				{
					for (json& component : *normal)
					{
						// As a double: a literal 1 in a pattern parses as unsigned.
						if (component.is_number())
							component = -component.get<double>();
					}
				}
				entry.Flipped = std::move(flipped);
			}
			return result;
		}

		bool IsContactEvent(const json& event)
		{
			return event.value("event", "") == AutomationEventTypeName(AutomationEventType::Collision) && event.contains("a") && event.contains("b");
		}

		// Whether an event has every field of the pattern (or of its flipped form, for a contact), compared as
		// assert.component compares fields.
		bool EventMatches(const json& event, const EventPattern& pattern, double tolerance)
		{
			if (JsonContainsSubset(event, *pattern.Pattern, tolerance))
				return true;
			return pattern.Flipped && IsContactEvent(event) && JsonContainsSubset(event, *pattern.Flipped, tolerance);
		}

		// Gives each pattern an event of its own (maximum bipartite matching, Kuhn's algorithm), so a pattern listed twice
		// needs two matching events, and a general pattern listed first does not take the only event a later, more specific
		// one matches. Returns the index of a pattern left without an event, or nullopt when every pattern has one.
		std::optional<size_t> FindUnmatchedPattern(std::span<const json> events, std::span<const EventPattern> patterns, double tolerance)
		{
			std::vector<std::vector<size_t>> candidates(patterns.size());
			for (size_t pattern = 0; pattern < patterns.size(); pattern++)
			{
				for (size_t event = 0; event < events.size(); event++)
				{
					if (EventMatches(events[event], patterns[pattern], tolerance))
						candidates[pattern].push_back(event);
				}
				if (candidates[pattern].empty())
					return pattern;
			}
			constexpr size_t Unowned = std::numeric_limits<size_t>::max();
			std::vector<size_t> owner(events.size(), Unowned);
			std::vector<bool> visited;
			// Finds an event for `pattern`, moving earlier patterns to other events of theirs when that frees one.
			const auto assign = [&](const auto& self, size_t pattern) -> bool {
				for (size_t event : candidates[pattern])
				{
					if (visited[event])
						continue;
					visited[event] = true;
					if (owner[event] == Unowned || self(self, owner[event]))
					{
						owner[event] = pattern;
						return true;
					}
				}
				return false;
			};
			for (size_t pattern = 0; pattern < patterns.size(); pattern++)
			{
				visited.assign(events.size(), false);
				if (!assign(assign, pattern))
					return pattern;
			}
			return std::nullopt;
		}

		// Why the closest event of the pattern's kind does not match it ("a.name is \"Marble\", expected \"Goal\"", in the
		// event's own terms), for the failure message; empty when no queued event is of that kind. Runs after the poll,
		// outside the hub's lock.
		std::string ClosestEventMismatch(std::span<const json> events, const EventPattern& pattern, double tolerance)
		{
			std::optional<std::vector<std::string>> closest;
			const auto kind = pattern.Pattern->find("event");
			for (const json& event : events)
			{
				if (kind != pattern.Pattern->end() && event.value("event", json()) != *kind)
					continue;
				std::vector<std::string> mismatches;
				MatchJsonSubset(event, *pattern.Pattern, tolerance, "", mismatches);
				if (pattern.Flipped && IsContactEvent(event))
				{
					std::vector<std::string> flippedMismatches;
					MatchJsonSubset(event, *pattern.Flipped, tolerance, "", flippedMismatches);
					if (flippedMismatches.size() < mismatches.size())
						mismatches = std::move(flippedMismatches);
				}
				if (!closest || mismatches.size() < closest->size())
					closest = std::move(mismatches);
			}
			return closest ? fmt::format("{}", fmt::join(*closest, "; ")) : std::string();
		}

		void RegisterEvents(CommandRegistry& registry)
		{
			json types = Schema::String("");
			types["enum"] = json::array();
			for (const char* name : AutomationEventTypeNames())
				types["enum"].push_back(name);
			const std::string typeList = fmt::format("{}", fmt::join(AutomationEventTypeNames(), ", "));
			json typesSchema = { { "type", "array" }, { "description", "array of event types: " + typeList + " (default all)" }, { "items", types }, { "minItems", 1 } };
			std::vector<std::string> levels;
			for (auto level = static_cast<int>(LogLevel::Trace); level <= static_cast<int>(LogLevel::Critical); level++)
				levels.emplace_back(LogLevelName(static_cast<LogLevel>(level)));
			json minLevel = Schema::String(fmt::format("string, lowest log level delivered: {} (default info)", fmt::join(levels, ", ")));
			minLevel["enum"] = levels;
			Add(registry, "events.subscribe", "Starts collecting events: log lines, script errors, play-state changes (play, simulate, stop; pause and resume from the editor's Pause button), collisions (collisionBegin/End and triggerEnter/Exit, in Play and Simulate) and animation events (Animator Events markers playback crossed: name, clip, entity, frame; in Play and Simulate). On the editor's TCP server a line-protocol connection receives them as {\"event\": ...} lines without 'ok' (and {\"event\": \"dropped\"} when it falls behind); MCP and CLI clients read them with events.poll. A TCP connection's subscriptions end with it. Returns {subscription, types, minLevel, streamed}.", Schema::Object({ { "types", typesSchema }, { "minLevel", Schema::Nullable(minLevel) } }), [](AutomationSession& session, const json& params) {
				AutomationEventHub::Filter filter;
				if (auto it = params.find("types"); it != params.end())
					filter.Types = ParseEventTypes(*it);
				if (const std::string level = OptionalString(params, "minLevel"); !level.empty())
				{
					const std::optional<LogLevel> parsed = LogLevelFromName(level);
					if (!parsed)
						throw CommandError("unknown log level '" + level + "'");
					filter.MinLevel = *parsed;
				}
				const AutomationSession::EventClient& client = session.GetEventClient();
				const std::optional<uint64_t> id = session.GetEvents()->Subscribe(filter, client.Owner, client.Delivery);
				if (!id)
					throw CommandError(fmt::format("too many event subscriptions (at most {}); end one with events.unsubscribe", AutomationEventHub::MaxSubscriptions));
				json names = json::array();
				for (uint32_t type = 0; type < static_cast<uint32_t>(AutomationEventType::Count); type++)
				{
					if (filter.Types & AutomationEventHub::TypeBit(static_cast<AutomationEventType>(type)))
						names.push_back(AutomationEventTypeName(static_cast<AutomationEventType>(type)));
				}
				return json{ { "subscription", *id }, { "types", names }, { "minLevel", LogLevelName(filter.MinLevel) }, { "streamed", client.Delivery == AutomationEventDelivery::Streamed } };
			});

			json assertTypes = typesSchema;
			assertTypes["description"] = "array of event types: fail unless the first 'max' queued events include each; on failure they stay queued";
			const json expect = { { "type", "array" }, { "description", "array of event patterns, e.g. {\"event\": \"collision\", \"type\": \"triggerEnter\", \"a\": {\"name\": \"Goal\"}}: fail unless each pattern matches an event of its own among the first 'max' queued (list a pattern twice to require two such events). A pattern names at least one field (its 'event', if given, is an event type) and matches an event that has every field it names, compared as assert.component compares fields (floats within 'tolerance'). A collision pattern that names a or b also matches the contact seen from the other body (a and b swapped, 'normal', which points from b toward a, reversed): contacts list their bodies in ID order. On failure the events stay queued, and when no event matches a pattern the error says how the closest event of its kind differs" }, { "items", { { "type", "object" } } }, { "minItems", 1 }, { "maxItems", MaxEventPatterns } };
			Add(registry, "events.poll", fmt::format("Returns (and removes) up to 'max' events a polled subscription collected, oldest first, with 'dropped': how many were lost because more than {} were waiting.", AutomationEventHub::MaxQueuedEvents), Schema::Object({ { "subscription", Schema::Integer("integer, from events.subscribe", 1, UINT64_MAX) }, { "max", Schema::Nullable(Schema::Integer(fmt::format("integer (default {})", AutomationEventHub::MaxQueuedEvents), 1, AutomationEventHub::MaxQueuedEvents)) }, { "assertTypes", assertTypes }, { "expect", expect }, { "tolerance", Schema::Nullable(Schema::Number(fmt::format("number, floats in 'expect' patterns match within it (default {}); needs 'expect'", DefaultMatchTolerance), 0.0, MaxMatchTolerance)) } }, { "subscription" }), [typeList](AutomationSession& session, const json& params) {
				const uint64_t id = Require(params, "subscription").get<uint64_t>();
				const auto max = static_cast<size_t>(OptionalInteger(params, "max", AutomationEventHub::MaxQueuedEvents, 1, AutomationEventHub::MaxQueuedEvents));
				uint32_t requiredTypes = 0;
				if (auto it = params.find("assertTypes"); it != params.end())
					requiredTypes = ParseEventTypes(*it);
				const json patterns = params.value("expect", json::array());
				for (size_t i = 0; i < patterns.size(); i++)
				{
					// An empty pattern would match any event, and a misspelled event type none: both are mistakes.
					if (patterns[i].empty())
						throw CommandError(fmt::format("parameter 'expect[{}]' names no field", i));
					if (const auto kind = patterns[i].find("event"); kind != patterns[i].end() && (!kind->is_string() || !AutomationEventTypeFromName(kind->get<std::string>())))
						throw CommandError(fmt::format("parameter 'expect[{}].event' must be one of: {}", i, typeList));
				}
				if (HasParam(params, "tolerance") && patterns.empty())
					throw CommandError("parameter 'tolerance' needs 'expect'");
				const double tolerance = MatchTolerance(params);
				// Built before the poll: the check runs under the hub's lock.
				const std::vector<EventPattern> expected = MakeEventPatterns(patterns);
				AutomationEventHub::PollCheck check;
				if (requiredTypes != 0 || !expected.empty())
				{
					check = [requiredTypes, &expected, tolerance](std::span<const json> queued) {
						return HasEveryEventType(queued, requiredTypes) && !FindUnmatchedPattern(queued, expected, tolerance);
					};
				}
				std::vector<json> events;
				uint64_t dropped = 0;
				switch (session.GetEvents()->Poll(id, max, events, dropped, check))
				{
					case AutomationEventHub::PollStatus::UnknownSubscription:
						throw CommandError("no event subscription " + std::to_string(id));
					case AutomationEventHub::PollStatus::Streamed:
						throw CommandError("event subscription " + std::to_string(id) + " streams to its connection; it cannot be polled");
					case AutomationEventHub::PollStatus::Rejected:
					{
						// Only the first 'max' are examined; say so when there may be more.
						const std::string queued = fmt::format("among the {}{} queued ({}; {} dropped); they stay queued", events.size() == max ? "first " : "", events.size(), DescribeEventCounts(events), dropped);
						if (const std::vector<std::string> missing = MissingEventTypes(events, requiredTypes); !missing.empty())
							throw CommandError(fmt::format("assertion failed: no {} event {}", fmt::join(missing, ", "), queued));
						// The check saw exactly these events, so a pattern is left over.
						const std::optional<size_t> unmatched = FindUnmatchedPattern(events, expected, tolerance);
						if (!unmatched)
							throw CommandError("internal error: events.poll rejected events that satisfy every check");
						const EventPattern& pattern = expected[*unmatched];
						const std::string text = pattern.Pattern->dump(-1, ' ', false, json::error_handler_t::replace);
						if (!std::ranges::any_of(events, [&](const json& event) { return EventMatches(event, pattern, tolerance); }))
						{
							const std::string closest = ClosestEventMismatch(events, pattern, tolerance);
							throw CommandError(fmt::format("assertion failed: no event matches {} {}{}", text, queued, closest.empty() ? "" : "; closest: " + closest));
						}
						throw CommandError(fmt::format("assertion failed: {} needs an event of its own, but the events it matches are taken by the other patterns {}", text, queued));
					}
					case AutomationEventHub::PollStatus::Ok:
						break;
				}
				return json{ { "events", events }, { "dropped", dropped } };
			});

			Add(registry, "events.unsubscribe", "Ends an event subscription (any client's) and discards its queued events.", Schema::Object({ { "subscription", Schema::Integer("integer, from events.subscribe", 1, UINT64_MAX) } }, { "subscription" }), [](AutomationSession& session, const json& params) {
				const uint64_t id = Require(params, "subscription").get<uint64_t>();
				if (!session.GetEvents()->Unsubscribe(id))
					throw CommandError("no event subscription " + std::to_string(id));
				return json::object();
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
		RegisterAssertions(registry);
		RegisterEvents(registry);
	}

}
