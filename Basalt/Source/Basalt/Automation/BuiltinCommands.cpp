#include "Basalt/Animation/AnimatedModel.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Asset/ImageUtils.h"
#include "Basalt/Asset/TextureSource.h"
#include "Basalt/Automation/AutomationSession.h"
#include "Basalt/Automation/CommandRegistry.h"
#include "Basalt/Automation/CommandSchema.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Input.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Project/Exporter.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Project/ProjectValidator.h"
#include "Basalt/Project/SmokeTest.h"
#include "Basalt/Renderer/RenderMath.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"
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
#include <optional>
#include <system_error>
#include <unordered_map>

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
				case ProcessOutcome::Failed:
					break;
			}
			return { { "passed", result.Passed }, { "outcome", outcome }, { "exitCode", exitCode }, { "summary", result.Summary }, { "log", LastLines(result.Process.Output, LogLines) } };
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
			GameUI::HostState view;
			if (!readVec2("origin", view.ViewportOrigin))
				throw CommandError("'origin' must be [x, y]");
			if (!readVec2("size", view.ViewportSize) || !(view.ViewportSize.x > 0.0f && view.ViewportSize.y > 0.0f))
				throw CommandError("'size' must be [width, height] with positive values");
			view.PointerEnabled = OptionalBool(params, "pointerEnabled", true);
			view.NavigationEnabled = OptionalBool(params, "navigationEnabled", true);
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

			Add(registry, "component.types", "Lists every component type with its fields (for component.set).", Schema::Object(), [](AutomationSession&, const json&) {
				json types = json::array();
				for (const ComponentInfo& info : ComponentRegistry::GetAll())
					types.push_back({ { "name", info.Name }, { "fields", info.Fields }, { "core", info.IsCore } });
				return json{ { "components", types } };
			});

			Add(registry, "log.get", "Returns engine log messages logged after index 'since'.", Schema::Object({ { "since", Schema::Nullable(Schema::Integer("integer, cursor from a previous call (default 0)", 0, INT64_MAX)) }, { "errorsOnly", Schema::Nullable(Schema::Boolean("bool")) } }), [](AutomationSession&, const json& params) {
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
				Project::SetActive(project);
				AssetManager::Clear();
				PhysicsWorld::ClearMeshShapeCache();
				session.SetEditScene(CreateRef<Scene>("Untitled"), "");
				return json{ { "directory", project->GetDirectory().string() } }; }, CommandPolicy::EditBoundary);

			Add(registry, "project.open", "Opens a project (directory or .bproject file) and its start scene.", Schema::Object({ { "path", Schema::String("string", true) } }, { "path" }), [](AutomationSession& session, const json& params) {
				std::string error;
				Ref<Project> project = Project::Load(RequireString(params, "path"), error);
				if (!project)
					throw CommandError(error);
				Project::SetActive(project);
				AssetManager::Clear();
				PhysicsWorld::ClearMeshShapeCache();
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
			Add(registry, "project.set", "Changes project settings and saves the project file.", Schema::Object({ { "graphics", Schema::Nullable(graphicsSchema) }, { "name", Schema::Nullable(Schema::String("string")) }, { "startScene", Schema::Nullable(Schema::String("string, project-relative scene")) }, { "windowWidth", Schema::Nullable(Schema::Integer("integer", ProjectConfig::MinWindowSize, ProjectConfig::MaxWindowSize)) }, { "windowHeight", Schema::Nullable(Schema::Integer("integer", ProjectConfig::MinWindowSize, ProjectConfig::MaxWindowSize)) }, { "fullscreen", Schema::Nullable(Schema::Boolean("bool")) }, { "renderScale", Schema::Nullable(Schema::Number(fmt::format("number in [{}, {}], fraction of the window resolution the scene renders at (the highest scale with dynamic resolution)", MinRenderScale, MaxRenderScale), MinRenderScale, MaxRenderScale)) }, { "dynamicResolution", Schema::Nullable(Schema::Boolean("bool, lower the render scale when the GPU cannot hold targetFrameRate (needs GPU timer queries)")) }, { "targetFrameRate", Schema::Nullable(Schema::Number(fmt::format("number in [{}, {}]; dynamic resolution keeps the scene's GPU time within this frame rate's frame time", MinTargetFrameRate, MaxTargetFrameRate), MinTargetFrameRate, MaxTargetFrameRate)) }, { "minRenderScale", Schema::Nullable(Schema::Number(fmt::format("number in [{}, {}], lowest scale dynamic resolution uses; it must be below renderScale, or dynamic resolution is inactive (a warning)", MinRenderScale, MaxRenderScale), MinRenderScale, MaxRenderScale)) }, { "physicsLayers", json{ { "type", "object" }, { "description", "object {Names: [\"Default\", ...], IgnoredPairs: [[a, b], ...]}, replaces all layers; applies from the next play" } } }, { "randomSeed", Schema::Nullable(Schema::Integer("integer in [0, 4294967295], seeds Lua's random numbers, particle emitters and runtime entity UUIDs; applies from the next play", 0, UINT32_MAX)) }, { "fixedTimestep", Schema::Nullable(json{ { "type", "number" }, { "description", fmt::format("number of seconds in (0, {}], the physics step of every scene that does not override it (scene.settings Physics.FixedTimestep); applies from the next play", MaxFixedTimestep) }, { "exclusiveMinimum", 0.0 }, { "maximum", MaxFixedTimestep } }) } }), [](AutomationSession& session, const json& params) {
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

				project->GetConfig() = std::move(config);
				std::string error;
				if (!project->Save(error))
					throw CommandError(error);
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

			Add(registry, "project.validate", "Checks that the project would export and start: settings, start scene, every scene and prefab (loads; referenced asset files exist inside Assets/ with the exact letter case; a bare file name without a folder only warns), every Lua script (compiles) and the scripts Script components use (load as script classes). Returns {valid, errors, warnings, omitted, issues: [{severity, file, message}]}.", Schema::Object({ { "assertValid", Schema::Nullable(Schema::Boolean("bool: fail the command when there is an error-severity issue")) } }), [](AutomationSession&, const json& params) {
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
				json response{ { "executable", result.ExecutablePath.string() }, { "files", result.FileCount }, { "validation", validation } };
				if (smokeFrames > 0)
				{
					const SmokeTestResult smoke = RunSmokeTest(result.ExecutablePath, static_cast<uint32_t>(smokeFrames), SecondsToMilliseconds(smokeSeconds));
					if (!smoke.Passed)
						throw CommandError("exported to '" + result.ExecutablePath.parent_path().string() + "', but the " + SmokeTestFailure(smoke));
					response["smokeTest"] = SmokeTestJson(smoke);
				}
				return response;
			});

			Add(registry, "project.smoke_test", "Runs an exported game headless (no GPU needed) for up to N frames at the runtime's fixed 1/60 s step and reports whether it exited cleanly: exit code 0 and no script errors within the timeout. Returns {passed, outcome: exited|crashed|timedOut|failed, exitCode (null unless it exited or crashed), summary, log}.", Schema::Object({ { "executable", Schema::String("string, the exported game's executable (project.export's executable)", true) }, { "frames", Schema::Nullable(Schema::Integer("integer (default 120)", 1, MaxSmokeTestFrames)) }, { "timeout", Schema::Nullable(Schema::Number("number of seconds (default 60)", 1, MaxSmokeTestSeconds)) }, { "assertPassed", Schema::Nullable(Schema::Boolean("bool: fail the command when the smoke test fails")) } }, { "executable" }), [](AutomationSession&, const json& params) {
				const std::filesystem::path executable = RequireString(params, "executable");
				std::error_code error;
				if (!std::filesystem::is_regular_file(executable, error))
					throw CommandError("no executable at '" + executable.string() + "'");
				const auto frames = static_cast<uint32_t>(OptionalInteger(params, "frames", DefaultSmokeTestFrames, 1, MaxSmokeTestFrames));
				const SmokeTestResult smoke = RunSmokeTest(executable, frames, SecondsToMilliseconds(OptionalNumber(params, "timeout", DefaultSmokeTestSeconds, 1, MaxSmokeTestSeconds)));
				if (OptionalBool(params, "assertPassed", false) && !smoke.Passed)
					throw CommandError(SmokeTestFailure(smoke));
				return SmokeTestJson(smoke);
			});
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
				session.SetScenePath(path);
				session.ClearDirty();
				return json{ { "path", path } }; }, CommandPolicy::EditBoundary);

			Add(registry, "scene.hash", "Hash of the current scene state (the play copy while playing); equal states give equal hashes, whatever the entity UUIDs.", Schema::Object({ { "expectHash", Schema::String("string: fail unless the hash equals this one (16 lowercase hex digits)", true) } }), [](AutomationSession& session, const json& params) {
				const std::string hash = SceneSerializer::ComputeStateHash(RequireScene(session));
				CheckExpectedHash(params, hash);
				return json{ { "hash", hash } };
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
					const json view = { { "origin", { host.ViewportOrigin.x, host.ViewportOrigin.y } }, { "size", { host.ViewportSize.x, host.ViewportSize.y } }, { "pointerEnabled", host.PointerEnabled }, { "navigationEnabled", host.NavigationEnabled } };
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
				const ComponentInfo* info = ComponentRegistry::Find(name);
				if (!info)
					throw CommandError("unknown component '" + name + "' (valid: " + ComponentRegistry::GetNameList() + ")");
				if (!info->Has(entity))
					throw CommandError("entity '" + entity.GetName() + "' has no " + name + " component");
				return info->Serialize(entity);
			});

			Add(registry, "component.remove", "Removes a component.", Schema::Object({ { "entity", Schema::Entity("ID or name") }, { "component", Schema::String("string", true) } }, { "entity", "component" }), [](AutomationSession& session, const json& params) {
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
			Add(registry, "asset.list", "Lists project asset files, optionally filtered by extensions (e.g. ['.lua']).", Schema::Object({ { "extensions", json{ { "type", "array" }, { "description", "array of strings" }, { "items", Schema::String() } } } }), [](AutomationSession&, const json& params) {
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

			Add(registry, "asset.write", "Writes a text asset (script, prefab, scene...) to a project-relative path.", Schema::Object({ { "path", Schema::String("string", true) }, { "content", Schema::String("string") } }, { "path", "content" }), [](AutomationSession&, const json& params) {
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
					throw CommandError("script errors: " + errors.dump());
				json result = { { "time", scene.GetTime() }, { "frame", scene.GetFrameCount() }, { "scriptErrors", errors }, { "quitRequested", scene.IsQuitRequested() }, { "scene", scene.GetName() }, { "scenePath", scene.GetPath() } };
				if (OptionalBool(params, "hash", false) || params.contains("expectHash"))
				{
					const std::string hash = SceneSerializer::ComputeStateHash(scene);
					CheckExpectedHash(params, hash);
					result["stateHash"] = hash;
				}
				return result;
			});

			Add(registry, "replay.record_start", "Starts recording input.key/input.mouse/input.gamepad commands with the play frame they arrive on. The mouse position and the injected gamepads (with what they hold) are recorded as they are on that frame, and the host's view (where mouse positions land on the UI canvas) whenever it changes, so replays map them the same way in any host. Physical gamepads are not recorded (replays run without them): they are listed in physicalGamepads.", Schema::Object(), [](AutomationSession& session, const json&) {
				session.StartRecording();
				json physical = json::array();
				for (uint32_t index = 0; index < Input::MaxGamepads; index++)
				{
					if (Input::GetGamepadSource(index) == GamepadSource::Device)
						physical.push_back(index);
				}
				if (!physical.empty())
					BS_CORE_WARN("replay.record_start: physical gamepads {} are connected; replays run without them, so their input will not replay", physical.dump());
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
				const json events = session.StopRecording();
				json replay = { { "Format", "BasaltReplay" }, { "Version", 1 }, { "Frames", frames }, { "RandomSeed", seed }, { "Events", events } };
				// The recorded run's final state: replay.run with assertHash checks a replay reproduces it.
				if (playing)
					replay["StateHash"] = SceneSerializer::ComputeStateHash(*playing);
				if (!FileSystem::WriteTextFile(Project::ResolvePath(path), replay.dump(1, '\t')))
					throw CommandError("cannot write '" + path + "'");
				return json{ { "path", path }, { "events", events.size() }, { "frames", frames } };
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
				struct RestoreHost
				{
					GameUI::HostState Host = GameUI::GetHostState();
					bool DeviceGamepads = Input::IsDeviceGamepadsEnabled();
					~RestoreHost()
					{
						GameUI::SetHostState(Host);
						Input::SetDeviceGamepadsEnabled(DeviceGamepads);
					}
				} restore;
				GameUI::SetHostState({});
				Input::SetDeviceGamepadsEnabled(false);
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
							else
								throw CommandError("unknown command '" + command + "'");
						}
						catch (const CommandError& e)
						{
							throw CommandError(fmt::format("'{}' event {} (frame {}, {}): {}", path, next, frame, command, e.what()));
						}
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

			Add(registry, "render.stats", "Returns the scene renderer's statistics: viewport and render size, render scale and dynamic resolution state, draw and culling counts, and the GPU time of each pass averaged over recent frames (editor / GPU hosts only).", Schema::Object(), [](AutomationSession& session, const json&) {
				if (!session.RenderStats)
					throw CommandError("render statistics need a GPU host (the editor); this host is '" + session.HostName + "'");
				json stats;
				std::string error;
				if (!session.RenderStats(stats, error))
					throw CommandError(error);
				return stats;
			});

			// Headless, so tests and agents can check renders from any host (golden-image tests use it).
			Add(registry, "image.compare", "Compares a PNG with a reference image within a tolerance; optionally writes a diff image and fails on mismatch.", Schema::Object({ { "actual", Schema::String("string, image path (relative paths are project-relative)", true) }, { "reference", Schema::String("string, image path", true) }, { "diff", Schema::Nullable(Schema::String("string, output .png, optional")) }, { "pixelThreshold", Schema::Nullable(Schema::Integer("integer 0-255, default 8", 0, 255)) }, { "maxPercent", Schema::Nullable(Schema::Number("number, default 0.5", 0.0, 100.0)) }, { "assertMatch", Schema::Nullable(Schema::Boolean("bool, default false")) } }, { "actual", "reference" }),
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
