#include "Breccia/Animation/AnimatedModel.h"
#include "Breccia/Animation/AnimationClipSet.h"
#include "Breccia/Asset/AssetKey.h"
#include "Breccia/Asset/AssetManager.h"
#include "Breccia/Asset/MaterialAsset.h"
#include "Breccia/Asset/MeshImporter.h"
#include "Breccia/Asset/ImageUtils.h"
#include "Breccia/Asset/TextureSource.h"
#include "Breccia/Asset/TextureCompression.h"
#include "Breccia/Automation/AutomationEvents.h"
#include "Breccia/Automation/AutomationSession.h"
#include "Breccia/Animation/AnimationGraph.h"
#include "Breccia/Animation/AnimationJointMap.h"
#include "Breccia/Animation/AnimationMask.h"
#include "Breccia/Animation/PropertyAnimation.h"
#include "Breccia/Automation/CommandRegistry.h"
#include "Breccia/Automation/CommandSchema.h"
#include "Breccia/Core/Assert.h"
#include "Breccia/Core/FileSystem.h"
#include "Breccia/Core/Input.h"
#include "Breccia/Core/JsonUtils.h"
#include "Breccia/Core/Log.h"
#include "Breccia/Core/MemoryTracker.h"
#include "Breccia/Core/PackArchive.h"
#include "Breccia/Network/NetworkSystem.h"
#include "Breccia/Network/SimulatedNetwork.h"
#include "Breccia/Physics/PhysicsWorld.h"
#include "Breccia/Project/Exporter.h"
#include "Breccia/Project/Packaging.h"
#include "Breccia/Project/Project.h"
#include "Breccia/Project/ProjectValidator.h"
#include "Breccia/Project/SmokeTest.h"
#include "Breccia/Renderer/RenderMath.h"
#include "Breccia/Scene/ComponentRegistry.h"
#include "Breccia/Scene/Entity.h"
#include "Breccia/Scene/Scene.h"
#include "Breccia/Scene/SceneDiff.h"
#include "Breccia/Scene/SceneSerializer.h"
#include "Breccia/Scene/Prefab.h"
#include "Breccia/Scripting/ScriptEngine.h"
#include "Breccia/UI/GameUI.h"
#include "Breccia/UI/UIFonts.h"
#include "Breccia/UI/UIInteraction.h"
#include "Breccia/UI/UILayout.h"

#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Breccia {

	namespace {

		// The "Format" marker of replay files (replay.record_stop writes it, replay.run requires it).
		constexpr const char* ReplayFormatName = "BrecciaReplay";

		using json = nlohmann::json;
		namespace Schema = CommandSchema;

		// The totals project.export and archive.inspect report for an archive.
		json ArchiveStatisticsToJson(const PackArchiveStatistics& statistics)
		{
			return { { "files", statistics.Entries }, { "compressed", statistics.CompressedEntries }, { "size", statistics.UncompressedSize }, { "archiveSize", statistics.ArchiveSize } };
		}

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

		// A retarget map as animator.retarget and animator.match_joints show it. `source` may be null with an empty map (a
		// library that did not load).
		json DescribeRetargetJoints(const MeshSource& model, const MeshSource* source, const RetargetMap& map)
		{
			json joints = json::array();
			for (size_t node = 0; node < map.SourceOf.size(); node++)
			{
				if (map.SourceOf[node] < 0)
					continue;
				joints.push_back({ { "target", model.Nodes[node].Name }, { "source", source->Nodes[map.SourceOf[node]].Name }, { "match", ToString(map.Matches[node]) } });
				if (const glm::vec3& offset = map.Offsets[node]; offset != glm::vec3(0))
					joints.back()["offset"] = { offset.x, offset.y, offset.z };
			}
			json root;
			if (map.TargetRoot >= 0)
				root = { { "target", model.Nodes[map.TargetRoot].Name }, { "source", source->Nodes[map.SourceRoot].Name }, { "scale", map.RootScale } };
			return { { "joints", std::move(joints) }, { "unmatchedHumanoidBones", Retargeting::UnmatchedHumanoidBones(model, map) }, { "root", std::move(root) } };
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
			json rotations = json::array();
			for (const auto& [field, type] : fieldTypes)
			{
				types[field] = type.ToString();
				if (type.QuaternionRotation)
					rotations.push_back(field);
				if (type.Kind != ComponentFieldKind::List)
					continue;
				const ComponentObjectSchema& element = *type.Element;
				json list = DescribeFields(element.Fields, element.FieldTypes, element.EnumOptions);
				list["name"] = element.Name;
				list["maxCount"] = type.MaxCount;
				if (!element.UniqueField.empty())
					list["uniqueField"] = element.UniqueField;
				if (!element.NormalizedTimeField.empty())
					list["normalizedTimeField"] = element.NormalizedTimeField;
				list["default"] = element.Default;
				lists[field] = std::move(list);
			}
			return { { "fields", fields }, { "types", types }, { "enums", enumOptions }, { "lists", lists }, { "quaternionRotations", rotations } };
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

		// In-process network client indices count every client a play session added (removed ones keep theirs).
		constexpr int64_t MaxClientIndex = static_cast<int64_t>(AutomationSession::MaxNetworkClients) * 1024;

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
		// Numeric IDs must name an existing entity too (0 is exempt: what it means is each field's own), except IDs the component already holds in list elements: a
		// list is written back whole, so an element whose entity was since deleted must not block an edit of another (it is
		// reported where it is used). Scene and prefab files stay lenient: they may be loaded before the entities they refer
		// to exist.
		json ResolveEntityFields(Scene& scene, const std::string& component, json data, Entity holder = {})
		{
			const ComponentInfo* info = ComponentRegistry::Find(component);
			if (!info || info->EntityFields.empty() || !data.is_object())
				return data;
			std::unordered_set<uint64_t> held;
			if (holder && info->Has(holder))
			{
				json current = info->Serialize(holder);
				for (const std::string& field : info->EntityFields)
					ForEachFieldValue(current, field, [&](const json& value, const std::string&) { held.insert(value.get<uint64_t>()); });
			}
			for (const std::string& field : info->EntityFields)
				ForEachFieldValue(data, field, [&](json& value, const std::string& label) {
					if (value.is_string() || (value.is_number_integer() && value != 0 && (label == field || !held.contains(value.get<uint64_t>()))))
						value = static_cast<uint64_t>(ResolveEntity(scene, value, label.c_str()).GetUUID());
				});
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
			if (FileSystem::PathFromUtf8(path).is_absolute())
				throw CommandError("asset paths must be project-relative (e.g. 'Assets/Scripts/Player.lua')");
			return path;
		}

		// A project-relative path a command imports from or writes to: under Assets/, without drive letters, backslashes or
		// symlinked components (a link could redirect a write outside the project). `subject` names it in errors.
		void RequireAssetsPath(const Project& project, const std::string& name, std::string_view subject)
		{
			const auto relative = FileSystem::PathFromUtf8(name).lexically_normal();
			if (relative.empty() || relative.has_root_path() || name.find(':') != std::string::npos || name.find('\\') != std::string::npos)
				throw CommandError("invalid project-relative path: " + name);
			if (*relative.begin() != "Assets")
				throw CommandError(fmt::format("{} must be under Assets/: {}", subject, name));
			std::filesystem::path entry;
			std::error_code error;
			if (FileSystem::FindSymlink(project.GetDirectory(), relative, entry, error))
			{
				if (error)
					throw CommandError("cannot inspect " + FileSystem::PathToUtf8(entry) + ": " + FileSystem::DescribeError(error));
				throw CommandError(fmt::format("symlinks are not allowed in {} paths: {}", subject, FileSystem::PathToUtf8(entry)));
			}
		}

		void AddSkippedEntryWarnings(const std::set<std::string>& entries, json& warnings)
		{
			for (const std::string& entry : entries)
				warnings.push_back(fmt::format("'{}': {}", entry, FileSystem::NonUtf8NameProblem));
		}

		// Adds the problems a material's texture references will have when it renders, as project.validate reports them
		// (there as errors): unresolved UUIDs, keys that are not textures, paths outside Assets/ or with backslashes, files
		// that do not exist or differ in letter case anywhere in the path (the game may run on a case-sensitive file
		// system), and embedded images the model does not have. Needs an active project.
		void AddMaterialTextureWarnings(const MaterialData& material, std::vector<std::string>& warnings)
		{
			const auto& project = Project::GetActive();
			for (const MaterialAsset::SlotInfo& slot : MaterialAsset::Slots)
			{
				const std::string& key = material.*slot.Map;
				const auto warn = [&](const std::string& problem) { warnings.push_back(fmt::format("{}: {}", slot.MapField, problem)); };
				if (key.empty())
					continue;
				if (AssetRegistry::IsUnresolvedKey(key))
				{
					warn(project->GetAssets().DescribeUnresolved(key));
					continue;
				}
				if (!MaterialAsset::IsTextureKey(key))
				{
					warn(fmt::format("'{}' is not a texture (an image file or <model>#image/<n>)", key));
					continue;
				}
				const std::string source(GetAssetSource(key));
				const std::filesystem::path relative = FileSystem::PathFromUtf8(source).lexically_normal();
				if (source.find('\\') != std::string::npos)
				{
					warn(fmt::format("'{}' uses backslashes; separate folders with / so it loads on every platform", key));
					continue;
				}
				if (relative.is_absolute() || relative.has_root_name() || relative.empty() || *relative.begin() != Project::AssetDirectoryName)
				{
					warn(fmt::format("'{}' is outside Assets/ and will not be exported", key));
					continue;
				}
				std::string actual, problem;
				switch (FileSystem::FindExactCase(project->GetDirectory(), relative, actual, problem))
				{
					case FileSystem::PathMatch::Missing:
						warn(fmt::format("'{}' does not exist", source));
						break;
					case FileSystem::PathMatch::DifferentCase:
						warn(fmt::format("'{}', but the file on disk is '{}' (letter case matters on Linux)", source, actual));
						break;
					case FileSystem::PathMatch::Unreadable:
						warn(fmt::format("'{}' cannot be checked: {}", source, problem));
						break;
					case FileSystem::PathMatch::Exact:
						if (!GetSubAssetFragment(key).empty())
						{
							const auto contents = MeshImporter::Inspect(Project::ResolvePath(source), problem);
							if (!contents)
								warn(fmt::format("'{}' is {}", source, problem));
							else if (const auto missing = MeshImporter::CheckSubAsset(*contents, key))
								warn(*missing);
						}
						break;
				}
			}
		}

		// A material file's text with UUID texture references (creating metadata for referenced files, as scene saves do).
		std::string MaterialFileText(const std::string& path, const MaterialData& material)
		{
			std::string error;
			const auto data = MaterialAsset::ToJson(material, true, error);
			if (!data)
				throw CommandError("cannot write material '" + path + "': " + error);
			return data->dump(1, '\t');
		}

		// Writes a material file's text and reloads it; `context` ends a failure's message.
		void WriteMaterialText(AutomationSession& session, const std::string& path, const std::string& text, std::string_view context = {})
		{
			std::string error;
			if (!FileSystem::WriteTextFile(Project::ResolvePath(path), text, error))
				throw CommandError(fmt::format("cannot write '{}': {}{}", path, error, context));
			session.OnFileWritten(path);
		}

		void RequireNewFile(const std::string& path, bool overwrite, std::string_view context = {})
		{
			if (!overwrite && FileSystem::Exists(Project::ResolvePath(path)))
				throw CommandError(fmt::format("'{}' already exists (pass overwrite=true to replace it){}", path, context));
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

		// Reloads that changed prefab files report how many instances merged them ("prefabInstances") and which could not.
		template<typename Warnings>
		void AddPrefabMerges(json& result, Warnings& warnings, const AssetReloadReport& report)
		{
			if (report.PrefabInstances == 0 && report.PrefabErrors.empty() && report.PrefabWarnings.empty())
				return;
			result["prefabInstances"] = report.PrefabInstances;
			for (const auto& error : report.PrefabErrors)
				warnings.push_back(error);
			for (const auto& warning : report.PrefabWarnings)
				warnings.push_back(warning);
		}

		// scene.open and project.open merge instances of prefabs saved since the scene was.
		json WithOutdatedPrefabMerges(AutomationSession& session, json result)
		{
			json warnings = json::array();
			AddPrefabMerges(result, warnings, session.MergeOutdatedPrefabInstances());
			if (!warnings.empty())
				result["warnings"] = std::move(warnings);
			return result;
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

		void ApplyTextInput(const json& params)
		{
			if (!params.contains("codepoint") || params["codepoint"].is_null())
				throw CommandError("missing parameter 'codepoint'");
			const auto codepoint = static_cast<uint32_t>(OptionalInteger(params, "codepoint", -1, 0, 0x10ffff));
			if (codepoint >= 0xd800 && codepoint <= 0xdfff)
				throw CommandError("'codepoint' must be a Unicode scalar value (surrogates are invalid)");
			if (codepoint < 0x20 || codepoint == 0x7f)
				throw CommandError("control characters are not text; use input.key");
			if (!Input::AddTextCodepoint(codepoint))
				throw CommandError("text input buffer is full this frame (16384 bytes)");
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
			if (auto scroll = params.find("scroll"); scroll != params.end())
			{
				glm::vec2 delta;
				if (!scroll->is_array() || scroll->size() != 2 || !JsonToFloat((*scroll)[0], delta.x) || !JsonToFloat((*scroll)[1], delta.y))
					throw CommandError("'scroll' must be [x, y]");
				Input::AddMouseScroll(delta);
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
			else if (info.Kind == UIWidgetKind::TextInput)
				summary["text"] = entity.GetComponent<UITextInputComponent>().Text;
			else if (info.Kind == UIWidgetKind::Dropdown)
				summary["selected"] = entity.GetComponent<UIDropdownComponent>().Selected;
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

			Add(registry, "component.types", "Lists every component type with its fields (for component.set): their names in order, each field's JSON type ('number', 'integer', 'boolean', 'string', 'object', 'entity', 'enum', an array such as 'number[3]', or 'object[]' for a list of objects), the values of enum fields, the fields that reference entities, and for each list field its element type under 'lists' ({name, fields, types, enums, maxCount, default, lists, and uniqueField when one string field must differ between elements}; a set replaces the whole list). entityFields names list element fields as 'List[].Field'. Includes quaternionRotations (Euler-degree fields sampled as quaternion orientations), supportsPropertyAnimation (false for PropertyAnimation, Prefab and Script) and, on physics configuration components, liveFields: the only fields tracks may animate, which physics applies in place.", Schema::Object(), [](AutomationSession&, const json&) {
				json types = json::array();
				for (const ComponentInfo& info : ComponentRegistry::GetAll())
				{
					json description = DescribeFields(info.Fields, info.FieldTypes, info.EnumOptions);
					description["name"] = info.Name;
					description["entityFields"] = info.EntityFields;
					description["core"] = info.IsCore;
					description["supportsPropertyAnimation"] = info.SupportsPropertyAnimation();
					if (info.SupportsPropertyAnimation() && info.AnimatableFields)
						description["liveFields"] = *info.AnimatableFields;
					types.push_back(std::move(description));
				}
				return json{ { "components", types } };
			});

			Add(registry, "log.get", "Returns engine log messages logged after index 'since'. With assertNoWarnings the command fails when any message since 'since' (whatever errorsOnly says) is a warning or worse, naming them, or when some were already dropped from the history (it keeps the last 2048 messages).", Schema::Object({ { "since", Schema::Nullable(Schema::Integer("integer, cursor from a previous call (default 0)", 0, INT64_MAX)) }, { "errorsOnly", Schema::Nullable(Schema::Boolean("bool")) }, { "assertNoWarnings", Schema::Nullable(Schema::Boolean("bool: fail the command when a message since 'since' is a warning or worse")) } }), [](AutomationSession&, const json& params) {
				const uint64_t since = static_cast<uint64_t>(OptionalInteger(params, "since", 0, 0, INT64_MAX));
				const bool errorsOnly = OptionalBool(params, "errorsOnly", false);
				const bool assertNoWarnings = OptionalBool(params, "assertNoWarnings", false);
				uint64_t next = 0;
				const auto history = Log::GetHistory().GetMessagesSince(since, next);
				json messages = json::array();
				std::vector<std::string> warnings;
				for (const LogMessage& message : history)
				{
					if (assertNoWarnings && message.Level >= LogLevel::Warn)
						warnings.push_back(fmt::format("[{}] {}", LogLevelName(message.Level), message.Text));
					if (errorsOnly && message.Level < LogLevel::Error)
						continue;
					messages.push_back({ { "level", LogLevelName(message.Level) }, { "logger", message.LoggerName }, { "text", message.Text } });
				}
				if (assertNoWarnings)
				{
					// The cursor stays usable after a failed assertion.
					const uint64_t wanted = next > since ? next - since : 0;
					if (const uint64_t dropped = wanted - history.size(); dropped > 0)
						throw CommandError(fmt::format("{} message(s) since {} were dropped from the log history, so they cannot be checked (next {})", dropped, since, next));
					if (!warnings.empty())
					{
						constexpr size_t Shown = 20;
						const size_t total = warnings.size();
						if (total > Shown)
							warnings.resize(Shown);
						throw CommandError(fmt::format("{} warning(s) or worse logged: {}{} (next {})", total, fmt::join(warnings, "; "), total > Shown ? fmt::format("; and {} more", total - Shown) : std::string(), next));
					}
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
			Add(registry, "project.create", "Creates a new project directory (Project.bproject + Assets/) and opens it.", Schema::Object({ { "path", Schema::String("string, project directory", true) }, { "name", Schema::Nullable(Schema::String(fmt::format("string of at most {} bytes", ProjectConfig::MaxTextLength))) } }, { "path" }), [](AutomationSession& session, const json& params) {
				std::string error;
				Ref<Project> project = Project::Create(FileSystem::PathFromUtf8(RequireString(params, "path")), OptionalString(params, "name", "Game"), error);
				if (!project)
					throw CommandError(error);
				ActivateProject(session, project);
				session.SetEditScene(CreateRef<Scene>("Untitled"), "");
				return json{ { "directory", FileSystem::PathToUtf8(project->GetDirectory()) } }; }, CommandPolicy::EditBoundary);

			Add(registry, "project.open", "Opens a project (directory or .bproject file) and its start scene. Instances of prefabs saved since the start scene was merge their prefab, keeping overrides ('prefabInstances'; instances that cannot merge are 'warnings').", Schema::Object({ { "path", Schema::String("string", true) } }, { "path" }), [](AutomationSession& session, const json& params) {
				std::string error;
				Ref<Project> project = Project::Load(FileSystem::PathFromUtf8(RequireString(params, "path")), error);
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
				return WithOutdatedPrefabMerges(session, { { "name", project->GetConfig().Name }, { "scene", startScene } }); }, CommandPolicy::EditBoundary);

			Add(registry, "project.info", "Returns the active project's settings.", Schema::Object(), [](AutomationSession&, const json&) {
				Ref<Project> project = RequireProject();
				const ProjectConfig& config = project->GetConfig();
				return json{
					{ "name", config.Name },
					{ "version", config.Version },
					{ "company", config.Company },
					{ "icon", config.Icon },
					{ "bundleIdentifier", MakeBundleIdentifier(config.Company, config.Name) },
					{ "directory", FileSystem::PathToUtf8(project->GetDirectory()) },
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
					{ "scriptInstructionLimit", config.ScriptInstructionLimit },
					{ "fixedTimestep", config.FixedTimestep },
					{ "inputActions", config.InputActions.ToJson() },
					{ "network", config.Network.ToJson() },
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
			Add(registry, "project.set", "Changes project settings and saves the project file.", Schema::Object({ { "graphics", Schema::Nullable(graphicsSchema) }, { "name", Schema::Nullable(Schema::String(fmt::format("string of at most {} bytes", ProjectConfig::MaxTextLength))) }, { "version", Schema::Nullable(Schema::String("string, MAJOR.MINOR.PATCH or MAJOR.MINOR.PATCH.BUILD (each 0-65535, no leading zeros): the version packaged exports carry")) }, { "company", Schema::Nullable(Schema::String(fmt::format("string of at most {} bytes, the publisher: Windows version resource, macOS copyright and bundle identifier", ProjectConfig::MaxTextLength))) }, { "icon", Schema::Nullable(Schema::String("string, project-relative PNG under Assets/ (at most 4096 px; 512 px or more recommended), the window and package icon; \"\" removes it")) }, { "startScene", Schema::Nullable(Schema::String("string, project-relative scene")) }, { "windowWidth", Schema::Nullable(Schema::Integer("integer", ProjectConfig::MinWindowSize, ProjectConfig::MaxWindowSize)) }, { "windowHeight", Schema::Nullable(Schema::Integer("integer", ProjectConfig::MinWindowSize, ProjectConfig::MaxWindowSize)) }, { "fullscreen", Schema::Nullable(Schema::Boolean("bool")) }, { "renderScale", Schema::Nullable(Schema::Number(fmt::format("number in [{}, {}], fraction of the window resolution the scene renders at (the highest scale with dynamic resolution)", MinRenderScale, MaxRenderScale), MinRenderScale, MaxRenderScale)) }, { "dynamicResolution", Schema::Nullable(Schema::Boolean("bool, lower the render scale when the GPU cannot hold targetFrameRate (needs GPU timer queries)")) }, { "targetFrameRate", Schema::Nullable(Schema::Number(fmt::format("number in [{}, {}]; dynamic resolution keeps the scene's GPU time within this frame rate's frame time", MinTargetFrameRate, MaxTargetFrameRate), MinTargetFrameRate, MaxTargetFrameRate)) }, { "minRenderScale", Schema::Nullable(Schema::Number(fmt::format("number in [{}, {}], lowest scale dynamic resolution uses; it must be below renderScale, or dynamic resolution is inactive (a warning)", MinRenderScale, MaxRenderScale), MinRenderScale, MaxRenderScale)) }, { "physicsLayers", json{ { "type", "object" }, { "description", "object {Names: [\"Default\", ...], IgnoredPairs: [[a, b], ...]}, replaces all layers; applies from the next play" } } }, { "randomSeed", Schema::Nullable(Schema::Integer("integer in [0, 4294967295], seeds Lua's random numbers, particle emitters and runtime entity UUIDs; applies from the next play", 0, UINT32_MAX)) }, { "scriptInstructionLimit", Schema::Nullable(Schema::Integer("integer in [0, 4294967295], how many VM instructions one call into Lua may run, and how many more bytes it may keep on the Lua heap (64 MiB at least), before it fails with an error, in the editor and the CLI (never in the runtime); 0 removes the limit, and the overhead of checking it; applies from the next play", 0, UINT32_MAX)) }, { "fixedTimestep", Schema::Nullable(json{ { "type", "number" }, { "description", fmt::format("number of seconds in (0, {}], the physics step of every scene that does not override it (scene.settings Physics.FixedTimestep); applies from the next play", MaxFixedTimestep) }, { "exclusiveMinimum", 0.0 }, { "maximum", MaxFixedTimestep } }) }, { "inputActions", json{ { "type", "object" }, { "description", "object {Actions: [{Name, Type: \"Button\" | \"Axis\", Bindings: [{Key | MouseButton | GamepadButton | GamepadAxis: name, Scale?, DeadZone?, Gamepad?}]}]}, replaces every input action and saves Input.binput (written only when the actions change); applies from the next play" } } }, { "network", json{ { "type", "object" }, { "description", fmt::format("object {{Port: 1-65535, MaxClients: 1-{}, TickRate: {}-{} snapshots per second, MaxBytesPerSecond: {}-{} per connection and direction, TimeoutSeconds: {}-{}, InterpolationDelay: 0-{} seconds a client shows remote entities behind the newest snapshot (0: as they arrive), MaxMessagesPerSecond: 1-{} script messages and RPCs per connection and direction}}, replaces the network session settings (keys left out take their defaults); applies to the next session", NetworkSettings::MaxClientLimit, NetworkSettings::MinTickRate, NetworkSettings::MaxTickRate, NetworkSettings::MinBytesPerSecond, NetworkSettings::MaxBytesPerSecondLimit, NetworkSettings::MinTimeout, NetworkSettings::MaxTimeout, NetworkSettings::MaxInterpolationDelay, NetworkSettings::MaxMessagesPerSecondLimit) } } } }), [](AutomationSession& session, const json& params) {
				Ref<Project> project = RequireProject();
				// Every value is read and validated before any is assigned, so a bad parameter leaves the
				// project untouched.
				ProjectConfig config = project->GetConfig();
				config.Name = OptionalString(params, "name", config.Name);
				config.Version = OptionalString(params, "version", config.Version);
				if (!ParseProjectVersion(config.Version))
					throw CommandError(fmt::format("parameter 'version' must be {} (got '{}')", VersionFormat, config.Version));
				config.Company = OptionalString(params, "company", config.Company);
				if (std::string error; !ValidateProjectText(config, error))
					throw CommandError("parameter " + error);
				// Checked only when it changes, so the other settings stay editable while the icon file is missing.
				if (const std::string icon = OptionalString(params, "icon", config.Icon); icon != config.Icon)
				{
					if (!icon.empty())
					{
						RequireAssetsPath(*project, ProjectRelative(icon), "the icon");
						std::string error;
						if (!LoadIconImage(project->GetDirectory() / FileSystem::PathFromUtf8(icon), error))
							throw CommandError("icon: " + error);
					}
					config.Icon = icon;
				}
				config.StartScene = OptionalString(params, "startScene", config.StartScene);
				config.WindowWidth = static_cast<uint32_t>(OptionalInteger(params, "windowWidth", config.WindowWidth, ProjectConfig::MinWindowSize, ProjectConfig::MaxWindowSize));
				config.WindowHeight = static_cast<uint32_t>(OptionalInteger(params, "windowHeight", config.WindowHeight, ProjectConfig::MinWindowSize, ProjectConfig::MaxWindowSize));
				config.Fullscreen = OptionalBool(params, "fullscreen", config.Fullscreen);
				config.Graphics.RenderScale = static_cast<float>(OptionalNumber(params, "renderScale", config.Graphics.RenderScale, MinRenderScale, MaxRenderScale));
				config.Graphics.DynamicResolution.Enabled = OptionalBool(params, "dynamicResolution", config.Graphics.DynamicResolution.Enabled);
				config.Graphics.DynamicResolution.TargetFrameRate = static_cast<float>(OptionalNumber(params, "targetFrameRate", config.Graphics.DynamicResolution.TargetFrameRate, MinTargetFrameRate, MaxTargetFrameRate));
				config.Graphics.DynamicResolution.MinScale = static_cast<float>(OptionalNumber(params, "minRenderScale", config.Graphics.DynamicResolution.MinScale, MinRenderScale, MaxRenderScale));
				config.RandomSeed = static_cast<uint32_t>(OptionalInteger(params, "randomSeed", config.RandomSeed, 0, UINT32_MAX));
				config.ScriptInstructionLimit = static_cast<uint32_t>(OptionalInteger(params, "scriptInstructionLimit", config.ScriptInstructionLimit, 0, UINT32_MAX));
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
				if (const auto it = params.find("network"); it != params.end())
				{
					std::string error;
					auto network = NetworkSettings::FromJson(*it, error);
					if (!network)
						throw CommandError("network: " + error);
					config.Network = *network;
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
					saveSucceeded = project->Save(error, params.contains("startScene") && !params["startScene"].is_null());
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
				session.AcknowledgeAssetMetadata();
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

			Add(registry, "project.validate", "Checks that the project would export and start: settings, start scene, every scene and prefab (loads; referenced asset files exist inside Assets/ with the exact letter case; a model's #image/N or #material/N exists in its glTF; path fields must name files inside Assets/ with forward slashes; other component text naming a missing Assets/ file is a warning), every animation graph and material file (parses; a material's maps are textures that exist), every Lua script (compiles) and the scripts Script components use (load as script classes). An entry of Assets/ whose name is not valid Unicode is a warning and is not checked. Returns {valid, errors, warnings, omitted, issues: [{severity, file, message}]}.", Schema::Object({ { "assertValid", Schema::Nullable(Schema::Boolean("bool: fail the command when there is an error-severity issue")) } }), [](AutomationSession&, const json& params) {
				const ValidationReport report = ValidateProject(*RequireProject());
				if (OptionalBool(params, "assertValid", false) && !report.IsValid())
					throw CommandError(DescribeValidationErrors(report));
				return ValidationReportJson(report);
			});

			Add(registry, "project.export", "Validates the project (unless validate is false), exports a standalone, distributable build of the game (runtime + assets; with package: true a platform package, see Docs/Shipping.md) and optionally smoke-tests it: runs the exported game headless for smokeTestFrames frames and fails unless it exits cleanly without script errors. Returns {executable (what the smoke test runs), package (what to distribute), files, validation, archive? ({files, compressed (files kept zstd-compressed), size (uncompressed bytes), archiveSize (bytes on disk), omitted (original images left out for their .ktx2 replacements)}), warnings?, smokeTest?}.", Schema::Object({ { "output", Schema::String("string, output directory", true) }, { "validate", Schema::Nullable(Schema::Boolean("bool (default true): refuse to export when project.validate finds an error")) }, { "smokeTestFrames", Schema::Nullable(Schema::Integer("integer (default 0: no smoke test), frames the exported game runs headless", 0, MaxSmokeTestFrames)) }, { "smokeTestTimeout", Schema::Nullable(Schema::Number("number of seconds (default 60) before the smoke test is stopped and fails", 1, MaxSmokeTestSeconds)) }, { "package", Schema::Nullable(Schema::Boolean("bool (default false): package for this platform with the project's icon, version and company: <Name>.app on macOS, an executable with icon and version resources on Windows, <Name>.AppDir plus <Name>-<version>-<arch>.AppImage on Linux (the AppImage needs appimagetool: appImageTool, $APPIMAGETOOL or PATH); <Name> is the executable's name: the project name with spaces as _ and other characters other than letters, digits, - and _ dropped")) }, { "appImageTool", Schema::Nullable(Schema::String("string, path of appimagetool for Linux packages (default: $APPIMAGETOOL, then PATH); requires package: true")) }, { "archive", Schema::Nullable(Schema::Boolean("bool (default false): pack Assets into one Assets.bpak archive (zstd-compressed where that helps, asset UUIDs in its index instead of .meta files) instead of copying the folder; see archive.inspect")) }, { "keepOriginals", Schema::Nullable(Schema::Boolean("bool (default false), with archive: keep the original images that a .ktx2 beside them replaces and no file mentions, for scripts that build such names at run time")) } }, { "output" }), [](AutomationSession& session, const json& params) {
				Ref<Project> project = RequireProject();
				ExportOptions options;
				options.Package = OptionalBool(params, "package", false);
				options.Archive = OptionalBool(params, "archive", false);
				options.KeepOriginals = OptionalBool(params, "keepOriginals", false);
				if (options.KeepOriginals && !options.Archive)
					throw CommandError("keepOriginals needs archive: true (a folder export copies every file)");
				options.AppImageTool = FileSystem::PathFromUtf8(OptionalString(params, "appImageTool", ""));
				if (!options.Package && !options.AppImageTool.empty())
					throw CommandError("appImageTool needs package: true");
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
				ExportResult result = Exporter::Export(*project, FileSystem::PathFromUtf8(RequireString(params, "output")), options);
				if (!result.Success)
				{
					// Warnings gathered before the failure (assets without UUIDs) still matter to whoever fixes it.
					std::string message = result.Error;
					for (const std::string& warning : result.Warnings)
						message += "\nwarning: " + warning;
					throw CommandError(message);
				}
				json response{ { "executable", FileSystem::PathToUtf8(result.ExecutablePath) }, { "package", FileSystem::PathToUtf8(result.PackagePath) }, { "files", result.FileCount }, { "validation", validation } };
				if (const auto& archive = result.ArchiveStatistics)
				{
					response["archive"] = ArchiveStatisticsToJson(*archive);
					response["archive"]["omitted"] = result.ArchiveOmitted;
				}
				if (!result.Warnings.empty())
					response["warnings"] = result.Warnings;
				if (smokeFrames > 0)
				{
					const SmokeTestResult smoke = RunSmokeTest(result.ExecutablePath, static_cast<uint32_t>(smokeFrames), SecondsToMilliseconds(smokeSeconds));
					if (!smoke.Passed())
					{
						std::string message = "exported to '" + FileSystem::PathToUtf8(result.ExecutablePath.parent_path()) + "', but the " + SmokeTestFailure(smoke);
						for (const std::string& warning : result.Warnings)
							message += "\nwarning: " + warning;
						throw CommandError(message);
					}
					response["smokeTest"] = SmokeTestJson(smoke);
				}
				return response;
			});

			Add(registry, "archive.inspect", "Lists a .bpak archive (project.export's Assets.bpak): every entry's path, asset UUID, size, stored size and compression. With verify: true, also reads every entry and checks its hash, failing on the first damaged one. Returns {entries: [{path, uuid (decimal string, null without one), size, storedSize, compression: store|zstd}], files, compressed, size (uncompressed bytes), archiveSize (bytes on disk), verified}.", Schema::Object({ { "path", Schema::String("string, the archive file; a relative path is relative to the current working directory", true) }, { "verify", Schema::Nullable(Schema::Boolean("bool (default false): read every entry and check its hash")) } }, { "path" }), [](AutomationSession&, const json& params) {
				const std::filesystem::path file = FileSystem::PathFromUtf8(RequireString(params, "path"));
				std::string error;
				const Ref<PackArchive> archive = PackArchive::Open(file, error);
				if (!archive)
					throw CommandError(error);
				const bool verify = OptionalBool(params, "verify", false);
				json entries = json::array();
				for (const PackArchive::Entry& entry : archive->GetEntries())
				{
					if (verify && !archive->Read(entry, error))
						throw CommandError("'" + FileSystem::PathToUtf8(file) + "': " + error);
					const bool zstd = entry.Compression == PackArchive::Method::Zstd;
					entries.push_back({ { "path", entry.Path }, { "uuid", entry.AssetID != 0 ? json(std::to_string(entry.AssetID)) : json(nullptr) }, { "size", entry.Size }, { "storedSize", entry.StoredSize }, { "compression", zstd ? "zstd" : "store" } });
				}
				json response = ArchiveStatisticsToJson(archive->GetStatistics());
				response["entries"] = std::move(entries);
				response["verified"] = verify;
				return response;
			});

			Add(registry, "project.smoke_test", "Runs an exported game headless (no GPU needed) for up to N frames at the runtime's fixed 1/60 s step and reports whether it exited cleanly: exit code 0 and no script errors within the timeout. Returns {passed, outcome: exited|crashed|timedOut|outputLimit (stopped after 256 MiB of output)|failed, exitCode (null unless it exited or crashed), summary, log}.", Schema::Object({ { "executable", Schema::String("string, the exported game's executable (project.export's executable); a relative path is relative to the current working directory", true) }, { "frames", Schema::Nullable(Schema::Integer("integer (default 120)", 1, MaxSmokeTestFrames)) }, { "timeout", Schema::Nullable(Schema::Number("number of seconds (default 60)", 1, MaxSmokeTestSeconds)) }, { "assertPassed", Schema::Nullable(Schema::Boolean("bool: fail the command when the smoke test fails")) } }, { "executable" }), [](AutomationSession&, const json& params) {
				// Relative paths resolve against the working directory of the process running the command.
				const std::filesystem::path executable = FileSystem::PathFromUtf8(RequireString(params, "executable"));
				std::error_code error;
				const bool isFile = std::filesystem::is_regular_file(executable, error);
				if (error && error != std::errc::no_such_file_or_directory)
					throw CommandError("cannot check '" + FileSystem::PathToUtf8(executable) + "': " + FileSystem::DescribeError(error));
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

			Add(registry, "scene.open", "Opens a scene file (project-relative path). Tracked instances of prefabs saved since the scene was merge their prefab, keeping overrides ('prefabInstances'; instances that cannot merge are 'warnings'); untracked ones are left for prefab.refresh.", Schema::Object({ { "path", Schema::String("string", true) } }, { "path" }), [](AutomationSession& session, const json& params) {
				const std::string path = ProjectRelative(RequireString(params, "path"));
				std::string error;
				Ref<Scene> scene = SceneSerializer::LoadScene(Project::ResolvePath(path), error);
				if (!scene)
					throw CommandError(error);
				session.SetEditScene(scene, path);
				// Prefabs saved since this scene was merge into their instances now, keeping overrides (the scene is then dirty).
				return WithOutdatedPrefabMerges(session, { { "name", scene->GetName() }, { "entities", scene->GetEntityCount() } }); }, CommandPolicy::EditBoundary);

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

			Add(registry, "scene.hash", "Hash of the current scene state (the play copy while playing), or of an in-process network client's (client, from net.connect); equal states give equal hashes, whatever the entity UUIDs. Peers agree once the networked entities are at rest: a client shows moving ones InterpolationDelay behind the host.", Schema::Object({ { "expectHash", Schema::String("string: fail unless the hash equals this one (16 lowercase hex digits)", true) }, { "client", Schema::Integer("integer, an in-process network client's index (net.connect)", 1, MaxClientIndex) } }), [](AutomationSession& session, const json& params) {
				Ref<Scene> client;
				if (params.contains("client"))
				{
					client = session.GetNetworkClient(static_cast<size_t>(OptionalInteger(params, "client", 0, 1, MaxClientIndex)));
					if (!client)
						throw CommandError("no such in-process network client (net.connect adds one)");
				}
				const std::string hash = SceneSerializer::ComputeStateHash(client ? *client : RequireScene(session));
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
				"Lays out the current scene's UI components (the play copy while playing) for a viewport of width x height pixels: the canvas size, every visible UI entity's untransformed rect [x, y, width, height] in canvas units in draw order, its column-major 3x3 transform and effective canvas clip (null when unrestricted) (widgets add their kind, values, target, callback and, while playing, state and focus), the host's view that maps the mouse onto the canvas for the widgets (`view`: origin and size in mouse coordinates and whether widgets take pointer and navigation input; headless, mouse positions are canvas units), and the problems that make UI draw or behave differently than authored (an image that does not load and draws its Color, a font that does not load and falls back to the built-in one or has glyphs that draw blank, an entity with several widget components, a widget callback whose target is missing or has no Script, an unusable composed transform, or an explicit navigation target that cannot currently take focus).", Schema::Object({ { "width", Schema::Nullable(Schema::Integer("integer, viewport pixels (default 1920)", 1, 65536)) }, { "height", Schema::Nullable(Schema::Integer("integer, viewport pixels (default 1080)", 1, 65536)) } }), [](AutomationSession& session, const json& params) {
					const auto width = static_cast<float>(OptionalInteger(params, "width", 1920, 1, 65536));
					const auto height = static_cast<float>(OptionalInteger(params, "height", 1080, 1, 65536));
					const glm::vec2 canvas = GameUI::GetCanvasSize({ width, height });
					json items = json::array();
					json problems = json::array();
					std::unordered_map<std::string, std::string> fontErrors;
					Scene& scene = RequireScene(session);
					std::vector<UILayoutProblem> layoutProblems;
					const auto layout = UILayout::Layout(scene, canvas, &layoutProblems);
					std::unordered_map<uint64_t, const UILayoutItem*> byEntity;
					for (const auto& item : layout)
						byEntity.emplace(static_cast<uint64_t>(item.Owner.GetUUID()), &item);
					for (const auto& problem : layoutProblems)
						problems.push_back({ { "id", static_cast<uint64_t>(problem.Owner.GetUUID()) }, { "name", problem.Owner.GetName() }, { "problem", problem.Message } });
					for (const UILayoutItem& item : layout)
					{
						const UIBox& rect = item.Rect;
						const uint64_t id = item.Owner.GetUUID();
						json entry = { { "id", id }, { "name", item.Owner.GetName() }, { "rect", { rect.Min.x, rect.Min.y, rect.Size.x, rect.Size.y } } };
						entry["clip"] = item.Clip ? json{ item.Clip->Min.x, item.Clip->Min.y, item.Clip->Size.x, item.Clip->Size.y } : json();
						entry["transform"] = json::array();
						for (int column = 0; column < 3; ++column)
							entry["transform"].push_back({ item.Transform[column].x, item.Transform[column].y, item.Transform[column].z });
						const auto report = [&](const std::string& problem) { problems.push_back({ { "id", id }, { "name", item.Owner.GetName() }, { "problem", problem } }); };
						if (const std::optional<UIWidgetInfo> widget = UILayout::GetWidget(item.Owner))
						{
							entry["widget"] = WidgetSummary(scene, item.Owner, *widget);
							if (widget->SeveralComponents)
								report(fmt::format("several widget components: it acts as its {} only", UIWidgetComponentName(widget->Kind)));
							if (const std::string problem = WidgetCallbackProblem(scene, item.Owner, *widget); !problem.empty())
								report(problem);
						}
						if (const auto* navigation = item.Owner.TryGetComponent<UINavigationComponent>())
						{
							const std::pair<const char*, UUID> directions[] = { { "Up", navigation->Up }, { "Down", navigation->Down }, { "Left", navigation->Left }, { "Right", navigation->Right } };
							for (const auto& [direction, targetID] : directions)
							{
								if (targetID == 0)
									continue;
								const auto prefix = fmt::format("UINavigation {} {}", direction, static_cast<uint64_t>(targetID));
								const Entity target = scene.GetEntityByUUID(targetID);
								if (!target)
								{
									report(prefix + " does not exist");
									continue;
								}
								const auto widget = UILayout::GetWidget(target);
								if (!widget || widget->Kind == UIWidgetKind::Panel || widget->Kind == UIWidgetKind::ScrollView)
									report(prefix + " is not a focusable widget");
								else if (!widget->Interactable)
									report(prefix + " is disabled");
								else if (const auto found = byEntity.find(static_cast<uint64_t>(targetID)); found == byEntity.end())
									report(prefix + " is hidden or has an unusable transform");
								else if (found->second->Rect.Size.x <= 0 || found->second->Rect.Size.y <= 0)
									report(prefix + " has a zero-area rect");
								else if (const auto& targetItem = *found->second; targetItem.Clip)
								{
									const UIBox visible = IntersectUIBoxes(TransformUIBox(targetItem.Rect, targetItem.Transform), *targetItem.Clip);
									if ((visible.Size.x <= 0 || visible.Size.y <= 0) && !UILayout::CanReveal(targetItem, byEntity))
										report(prefix + " is fully clipped and cannot be scrolled into view");
								}
							}
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

			Add(registry, "scene.get", "Returns the whole open scene as JSON with resolved asset paths (disk files persist UUID references).", Schema::Object(), [](AutomationSession& session, const json&) {
				return SceneSerializer::SerializeScene(RequireScene(session));
			});

			Add(registry, "scene.info", "Summary of the open scene: name, path, entity count, state, unsaved changes, undefined physics layers, animation authoring warnings and the physics step (fixedTimestep: the step in use; fixedTimestepOverride: the scene's own step, or null). With assertNoAnimationWarnings the command fails when there are animation authoring warnings, naming them.", Schema::Object({ { "assertNoAnimationWarnings", Schema::Nullable(Schema::Boolean("bool: fail the command when an entity has animation authoring warnings")) } }), [](AutomationSession& session, const json& params) {
				Scene& scene = RequireScene(session);
				const bool assertClean = OptionalBool(params, "assertNoAnimationWarnings", false);
				json animationWarnings = json::array();
				std::vector<std::string> named;
				for (auto handle : scene.GetRegistry().view<IDComponent>())
				{
					Entity entity(handle, &scene);
					for (const auto& warning : GetAnimationWarnings(entity))
					{
						animationWarnings.push_back({ { "entity", static_cast<uint64_t>(entity.GetUUID()) }, { "name", entity.GetName() }, { "message", warning } });
						if (assertClean)
							named.push_back(entity.GetName() + ": " + warning);
					}
				}
				if (!named.empty())
					throw CommandError(fmt::format("{} animation warning(s): {}", named.size(), fmt::join(named, "; ")));
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

			Add(registry, "animator.retarget", "How an Animator's named ClipLibraries map onto its model, resolved as playback resolves them but without baking (a listed clip can still be left out when it fails to bake or passes the 256 MiB per-Animator budget; the log reports it): per library its index in ClipLibraries, why it adds no clips (error: also a joint map that does not load or a reference clip that is not there), its joint map file (jointMap) and reference poses (reference, modelReference: {clip, time}, or null for the rest pose), the joint pairs (target, source, matched by joint map (Joints or the jointMap file), name or humanoid bone, and offset when the pair has one), the model's humanoid bones left unmatched, the root pair with its leg-length scale, the clips it adds, the clips earlier clips hide, and warnings. Read-only; works in edit mode.", Schema::Object({ { "entity", Schema::Entity("ID or name of an entity with an Animator") } }, { "entity" }), [](AutomationSession& session, const json& params) {
				Entity entity = RequireEntity(session, params);
				const auto* animator = entity.TryGetComponent<AnimatorComponent>();
				if (!animator)
					throw CommandError("entity '" + entity.GetName() + "' has no Animator");
				const auto model = AssetManager::GetMesh(animator->Mesh);
				if (!model)
					throw CommandError(fmt::format("Animator Mesh '{}' cannot load: {}", animator->Mesh, AssetManager::GetError(animator->Mesh)));
				std::vector<AnimationClipSet::Library> resolved = AnimationClipSet::Load(*animator);
				AnimationClipSet::Plan(*model, resolved);
				json libraries = json::array();
				const auto reference = [](const std::string& clip, float time) { return clip.empty() ? json() : json{ { "clip", clip }, { "time", time } }; };
				for (const AnimationClipSet::Library& library : resolved)
				{
					json entry = DescribeRetargetJoints(*model, library.Source.get(), library.Map);
					entry.update({
						{ "index", library.Index },
						{ "mesh", library.Entry.Mesh },
						{ "error", library.Error.empty() ? json() : json(library.Error) },
						{ "jointMap", library.Entry.JointMap.empty() ? json() : json(library.Entry.JointMap) },
						{ "reference", reference(library.Entry.ReferenceClip, library.Entry.ReferenceTime) },
						{ "modelReference", reference(library.Entry.ModelReferenceClip, library.Entry.ModelReferenceTime) },
						{ "clips", library.Clips },
						{ "hiddenClips", library.Shadowed },
						{ "warnings", library.Warnings },
					});
					libraries.push_back(std::move(entry));
				}
				return json{ { "entity", static_cast<uint64_t>(entity.GetUUID()) }, { "mesh", animator->Mesh }, { "libraries", std::move(libraries) } };
			});

			Add(registry, "animator.match_joints", "How a library model's skeleton maps onto a model's, for authoring a .bjointmap: the same matching as animator.retarget, on two models with no Animator (the pairs of jointMap first, then equal names, then humanoid bones). Returns {joints: [{target, source, match}], unmatchedHumanoidBones, root, warnings}, with offset on a pair that has one. Read-only; needs a project, not a scene.", Schema::Object({ { "source", Schema::String("Project-relative model the clips come from", true) }, { "target", Schema::String("Project-relative model an Animator plays them on", true) }, { "jointMap", Schema::String("Project-relative .bjointmap, optional", true) } }, { "source", "target" }), [](AutomationSession&, const json& params) {
				RequireProject();
				const auto load = [](const std::string& path) {
					const auto model = AssetManager::GetMesh(path);
					if (!model)
						throw CommandError("cannot load model '" + path + "': " + AssetManager::GetError(path));
					return model;
				};
				const auto source = load(params["source"].get<std::string>());
				const auto target = load(params["target"].get<std::string>());
				std::vector<AnimationRetargetJoint> pairs;
				if (const std::string path = OptionalString(params, "jointMap"); !path.empty())
				{
					const auto map = AssetManager::GetAnimationJointMap(path);
					if (!map)
						throw CommandError("cannot load joint map '" + path + "': " + AssetManager::GetError(path));
					pairs = map->Joints;
				}
				std::vector<std::string> warnings;
				const RetargetMap map = Retargeting::BuildMap(*target, *source, pairs, warnings);
				if (map.MappedCount() == 0)
					warnings.emplace_back("no joint matches the model's skeleton (pair them in a joint map)");
				json result = DescribeRetargetJoints(*target, source.get(), map);
				result["warnings"] = std::move(warnings);
				return result;
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
				if (!ComponentRegistry::AddOrPatch(entity, name, ResolveEntityFields(scene, name, params.value("data", json::object()), entity), error))
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

		json MoveAsset(AutomationSession& session, const json& params)
		{
			auto project = RequireProject();
			if (session.IsPlaying() || session.IsSimulating())
				throw CommandError("stop Play or Simulate before moving assets");
			const auto from = ProjectRelative(RequireString(params, "from"));
			const auto to = ProjectRelative(RequireString(params, "to"));
			std::string error;
			const bool movesStartScene = project->GetAssets().ReferencesSource(project->GetConfig().StartScene, from);
			const bool movesIcon = project->GetAssets().ReferencesSource(project->GetConfig().Icon, from);
			const bool movesOpenScene = project->GetAssets().ReferencesSource(session.GetScenePath(), from);
			if (!project->GetAssets().ValidateMove(from, to, error) || !project->GetAssets().Ensure(from, error))
				throw CommandError(error);

			struct Patch
			{
				UUID ID;
				const ComponentInfo* Info;
				json Data;
			};
			std::vector<Patch> patches;
			Scene validation;
			auto probe = validation.CreateEntity("AssetMoveValidation");
			if (auto scene = session.GetEditScene())
				for (auto entity : scene->GetAllEntitiesOrdered())
					for (const auto& info : ComponentRegistry::GetAll())
					{
						if (info.AssetFields.empty() || !info.Has(entity))
							continue;
						auto data = info.Serialize(entity);
						bool changed = false;
						for (const auto& field : info.AssetFields)
						{
							const bool present = ForEachFieldValue(data, field, [&](json& value, const std::string& label) {
								if (!value.is_string())
									throw CommandError("asset field did not serialize as a path: " + info.Name + "." + label);
								const auto& key = value.get_ref<const std::string&>();
								if (project->GetAssets().ReferencesSource(key, from))
								{
									value = to + std::string(GetSubAssetFragment(key));
									changed = true;
								}
							});
							if (!present)
								throw CommandError("asset field did not serialize: " + info.Name + "." + field);
						}
						// A baseline holds the source as last merged: migrate it too, or the next merge would read the move
						// as an instance override and keep the old path.
						if (info.Name == "Prefab" && data.contains("Baseline"))
							Prefab::ForEachBaselineAssetValue(data["Baseline"], [&](json& value, const json&, const std::string&) {
								if (value.is_string() && project->GetAssets().ReferencesSource(value.get_ref<const std::string&>(), from))
								{
									value = to + std::string(GetSubAssetFragment(value.get_ref<const std::string&>()));
									changed = true;
								}
								return true;
							});
						if (changed)
						{
							if (!info.Deserialize(probe, data, error))
								throw CommandError("cannot prepare open-scene asset references: " + error);
							patches.push_back({ entity.GetUUID(), &info, std::move(data) });
						}
					}

			// Prepare document replacements together. Newly created source metadata remains valid on failure.
			struct Migration
			{
				std::string Path, Before, After;
			};
			std::vector<Migration> migrations;
			// Scenes, prefabs and material files (textures, models' embedded images) hold references.
			std::set<std::string> unnamedEntries;
			const auto documents = AssetManager::ListAssets({ ".bscene", ".bprefab", std::string(MaterialAsset::Extension) }, &error, &unnamedEntries);
			std::vector<std::string> skipped;
			if (!error.empty())
				throw CommandError(error);
			// No key can open these and no JSON reference can name them, so they must not block the move; any references they
			// hold stay as they are.
			skipped.reserve(unnamedEntries.size());
			for (const std::string& entry : unnamedEntries)
				skipped.push_back(fmt::format("'{}' was not checked for references to this asset: {}", entry, FileSystem::NonUtf8NameProblem));
			for (const auto& path : documents)
			{
				const auto text = FileSystem::ReadTextFile(Project::ResolvePath(path));
				if (!text)
					throw CommandError("cannot read asset references in '" + path + "'");
				const std::string& fileText = *text;
				json data = ParseJson(fileText, error);
				const bool material = MaterialAsset::IsMaterialFile(path);
				if (data.is_discarded() || !data.is_object())
				{
					const std::string problem = "cannot migrate '" + path + "': " + (error.empty() ? "expected a JSON object" : error);
					// Nothing can load a broken material file (its references cannot resolve anyway): it must not block
					// moving unrelated assets. A scene or prefab might still be repaired by hand, so those stop the move.
					if (!material)
						throw CommandError(problem);
					BR_CORE_WARN("Asset move: {}", problem);
					skipped.push_back(problem);
					continue;
				}
				const auto original = data;
				if (!(material ? MaterialAsset::EncodeReferences(data, error, from) : SceneSerializer::EncodeAssetReferences(data, error, from)))
					throw CommandError(fmt::format("cannot migrate '{}': {}", path, error));
				if (data == original)
					continue;
				if (!material)
				{
					const auto version = data.find("Version");
					if (version != data.end() && (!version->is_number_integer() || *version < 1 || *version > SceneSerializer::FormatVersion))
						throw CommandError("cannot migrate unsupported document version in '" + path + "'");
					// A prefab from before nesting read its leftover Prefab components as nothing, and must go on doing so.
					const int64_t declared = version != data.end() ? version->get<int64_t>() : 0;
					if (FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(path)) == ".bprefab" && declared < Prefab::NestingVersion && data.contains("Entities") && data["Entities"].is_array())
						for (json& entity : data["Entities"])
							if (entity.is_object() && entity.contains("Components") && entity["Components"].is_object() && entity["Components"].erase("Prefab") > 0)
								skipped.push_back(fmt::format("{}: removed a Prefab component from '{}': this prefab's version ignored it, and the version it moves to would not", path, entity.value("Name", std::string("<unnamed>"))));
					// V1 wrote the old default as data. Preserve its 'follow the project' meaning before upgrading.
					if (version != data.end() && *version == 1 && data.contains("Physics") && data["Physics"].is_object())
					{
						auto& physics = data["Physics"];
						float step = 0;
						if (physics.contains("FixedTimestep") && JsonToFloat(physics["FixedTimestep"], step) && std::abs(step - DefaultFixedTimestep) < 1e-6f)
							physics.erase("FixedTimestep");
					}
					data["Version"] = SceneSerializer::FormatVersion;
				}
				migrations.push_back({ path, fileText, data.dump(1, '\t') });
			}
			if (movesStartScene || movesIcon)
			{
				const auto text = FileSystem::ReadTextFile(project->GetProjectFilePath());
				json data = text ? ParseJson(*text) : json();
				if (!text || !data.is_object())
					throw CommandError("cannot migrate the project's start scene and icon: cannot read Project.bproject");
				// The identity of the moved file (its UUID survives the move), so the project follows it.
				for (const auto& [key, moves] : { std::pair{ "StartScene", movesStartScene }, std::pair{ "Icon", movesIcon } })
				{
					if (!moves)
						continue;
					json reference;
					if (!project->GetAssets().Encode(from, reference, error))
						throw CommandError(error);
					data[key] = std::move(reference);
				}
				migrations.push_back({ Project::FileName, *text, data.dump(1, '\t') });
			}
			size_t written = 0;
			for (; written < migrations.size(); ++written)
				if (!FileSystem::WriteTextFile(Project::ResolvePath(migrations[written].Path), migrations[written].After, error))
					break;
			if (written != migrations.size() || !project->GetAssets().Move(from, to, error))
			{
				for (size_t i = 0; i < written; ++i)
				{
					std::string restoreError;
					if (!FileSystem::WriteTextFile(Project::ResolvePath(migrations[i].Path), migrations[i].Before, restoreError))
						error += "; cannot restore " + migrations[i].Path + ": " + restoreError;
				}
				throw CommandError(error);
			}

			// Disk state is committed. Finish host bookkeeping even if a component observer rejects a patch.
			if (movesStartScene)
				project->SetStartScene(to);
			if (movesIcon)
				project->GetConfig().Icon = to;
			if (movesOpenScene)
			{
				session.SetScenePath(to);
				if (session.GetEditScene())
					session.GetEditScene()->SetPath(to);
			}
			AssetManager::Clear();
			for (const auto& migration : migrations)
				// A scene/prefab can reference itself, so its migrated file may be the one just moved.
				session.AcknowledgeFile(migration.Path == from ? to : migration.Path);
			for (const auto& path : { from, to, from + ".meta", to + ".meta" })
				session.AcknowledgeFile(path);
			json warnings = skipped;
			if (!patches.empty())
				session.MarkDirty();
			for (const auto& patch : patches)
			{
				const auto previousWarnings = warnings.size();
				try
				{
					if (!patch.Info->Deserialize(session.GetEditScene()->GetEntityByUUID(patch.ID), patch.Data, error))
						warnings.push_back(patch.Info->Name + ": " + error);
				}
				catch (const std::exception& exception)
				{
					warnings.push_back(patch.Info->Name + ": " + exception.what());
				}
				if (warnings.size() != previousWarnings)
					BR_CORE_ERROR("Asset move '{}' -> '{}', entity {}: {}", from, to, static_cast<uint64_t>(patch.ID), warnings.back().get<std::string>());
			}
			if (session.OnAssetMoved)
				session.OnAssetMoved(from, to);
			json result = { { "from", from }, { "to", to } };
			if (!warnings.empty())
				result["warnings"] = std::move(warnings);
			return result;
		}

		std::string RequirePrefabPath(const json& params, const char* key = "path")
		{
			const auto path = FileSystem::NormalizeGeneric(ProjectRelative(RequireString(params, key)));
			RequireAssetsPath(*RequireProject(), path, "prefab");
			if (FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(path)) != ".bprefab")
				throw CommandError("expected a .bprefab file: " + path);
			return path;
		}

		Entity RequirePrefabRoot(AutomationSession& session, const json& params)
		{
			Entity root = RequireEntity(session, params);
			if (!root.HasComponent<PrefabComponent>())
				throw CommandError("entity '" + root.GetName() + "' is not a prefab instance root");
			return root;
		}

		Entity RequireEditablePrefabRoot(AutomationSession& session, const json& params)
		{
			Entity root = RequirePrefabRoot(session, params);
			if (session.IsPlaying())
				throw CommandError("stop Play or Simulate before editing prefab instances");
			return root;
		}

		// The edit-mode instance root an instance command changes. A nested instance is part of the instance holding it.
		Entity RequirePrefabInstance(AutomationSession& session, const json& params)
		{
			Entity root = RequireEditablePrefabRoot(session, params);
			if (const Entity enclosing = Prefab::EnclosingInstance(root))
				throw CommandError(fmt::format("'{}' is nested in the instance '{}': its overrides belong to that instance, so use its root", root.GetName(), enclosing.GetName()));
			return root;
		}

		// Reads the instance's prefab file and runs merge (a Prefab::Refresh or RevertOverride) with it. A partial merge
		// still changed the scene, so it is marked edited (and captured by history) before the error is reported.
		json PrefabOverridesResult(Entity root, const std::vector<std::string>& warnings = {});

		json MergeFromSourceFile(AutomationSession& session, Entity root, Prefab::RefreshMode mode, const std::string& field = {})
		{
			const std::string path = root.GetComponent<PrefabComponent>().Prefab;
			json source;
			std::string error;
			Prefab::ReadReport report;
			auto result = Prefab::RefreshResult::Rejected;
			if (Prefab::ReadSource(Project::ResolvePath(path), source, error, &report))
				result = field.empty() ? Prefab::Refresh(root, source, mode, error, &report.Warnings) : Prefab::RevertOverride(root, source, field, error, &report.Warnings);
			if (Prefab::ChangedScene(result))
				MarkEdited(session);
			if (Prefab::Failed(result))
				throw CommandError(fmt::format("instance '{}' of {}{}: {}", root.GetName(), path, result == Prefab::RefreshResult::Partial ? " was only partly merged (merge it again with prefab.refresh)" : "", error));
			return PrefabOverridesResult(root, report.Warnings);
		}

		json PrefabOverridesResult(Entity root, const std::vector<std::string>& warnings)
		{
			const auto& link = root.GetComponent<PrefabComponent>();
			json result = { { "prefab", link.Prefab }, { "tracked", Prefab::IsTracked(link) }, { "overrides", Prefab::ComputeOverrides(root) }, { "members", link.Members } };
			if (const Entity enclosing = Prefab::EnclosingInstance(root))
				result["enclosing"] = static_cast<uint64_t>(enclosing.GetUUID());
			if (!warnings.empty())
				result["warnings"] = warnings;
			return result;
		}

		// Writing a prefab merges it into the scene's instances. Those that could not merge are warnings, not errors,
		// because the file is already written.
		json PrefabWriteResult(const std::string& path, const AssetReloadReport& report, std::vector<std::string> warnings = {})
		{
			warnings.insert(warnings.end(), report.PrefabErrors.begin(), report.PrefabErrors.end());
			warnings.insert(warnings.end(), report.PrefabWarnings.begin(), report.PrefabWarnings.end());
			// A nested prefab's warning reaches a write from its read and from the merge after it.
			std::vector<std::string> unique;
			unique.reserve(warnings.size());
			for (std::string& warning : warnings)
				if (std::find(unique.begin(), unique.end(), warning) == unique.end())
					unique.push_back(std::move(warning));
			warnings = std::move(unique);
			return { { "path", path }, { "instances", report.PrefabInstances }, { "warnings", warnings } };
		}

		// The tree in SerializeEntityTree order: each entity before its children.
		void CollectTree(Entity entity, std::vector<Entity>& out)
		{
			out.push_back(entity);
			for (Entity child : entity.GetChildren())
				CollectTree(child, out);
		}

		void RegisterAssets(CommandRegistry& registry)
		{
			Add(registry, "asset.id", "Creates or reads the stable UUID sidecar of an existing asset under Assets/.", Schema::Object({ { "path", Schema::String("asset path", true) } }, { "path" }), [](AutomationSession& session, const json& params) {
				auto project = RequireProject();
				const auto path = ProjectRelative(RequireString(params, "path"));
				std::string error;
				const auto id = project->GetAssets().Ensure(path, error);
				if (!id)
					throw CommandError(error);
				session.AcknowledgeAssetMetadata();
				return json{ { "uuid", std::to_string(*id) }, { "path", path } };
			});

			Add(registry, "asset.resolve", "Resolves an asset UUID (decimal string) to its current project-relative path. Reads metadata without creating files.", Schema::Object({ { "uuid", Schema::String("nonzero decimal UUID", true) } }, { "uuid" }), [](AutomationSession&, const json& params) {
				auto project = RequireProject();
				std::string error;
				const auto id = AssetRegistry::ParseID(RequireString(params, "uuid"), error);
				if (!id || !project->GetAssets().Refresh(error))
					throw CommandError(error);
				const auto path = project->GetAssets().Find(*id);
				if (path.empty())
					throw CommandError("unresolved asset UUID " + std::to_string(*id));
				return json{ { "uuid", std::to_string(*id) }, { "path", path } };
			});

			auto move = [](AutomationSession& session, const json& params) {
				try
				{
					auto result = MoveAsset(session, params);
					session.AcknowledgeAssetMetadata();
					return result;
				}
				catch (...)
				{
					session.AcknowledgeAssetMetadata();
					throw;
				}
			};
			for (const auto* command : { "asset.move", "asset.rename" })
				Add(registry, command, "Moves one asset file and its .meta sidecar within Assets/. 'to' is the complete destination path. Creates metadata if needed, migrates scene, prefab and material-file references to this asset to UUIDs and updates the open edit scene and the project's start scene and icon. A scene or prefab that cannot be read stops the move (it might reference the asset); a material file that is not valid JSON, and a document or folder whose name is not valid Unicode, are skipped with a warning. Does not rewrite paths inside scripts or glTF files. Stop Play or Simulate first.", Schema::Object({ { "from", Schema::String("source path", true) }, { "to", Schema::String("destination path", true) } }, { "from", "to" }), move, CommandPolicy::EditBoundary);

			Add(registry, "asset.delete", "Deletes one asset file and its .meta identity. Existing UUID references become unresolved; deleting the project's start scene or icon is reported in warnings. Stop Play or Simulate first.", Schema::Object({ { "path", Schema::String("asset path", true) } }, { "path" }), [](AutomationSession& session, const json& params) {
				auto project = RequireProject();
				if (session.IsPlaying() || session.IsSimulating())
					throw CommandError("stop Play or Simulate before deleting assets");
				const auto path = ProjectRelative(RequireString(params, "path"));
				const ProjectConfig& config = project->GetConfig();
				const bool removesStartScene = project->GetAssets().ReferencesSource(config.StartScene, path);
				const bool removesIcon = project->GetAssets().ReferencesSource(config.Icon, path);
				std::string error;
				if (!project->GetAssets().Remove(path, error))
					throw CommandError(error);
				session.AcknowledgeFile(path + ".meta");
				json result = { { "path", path } };
				json warnings = json::array();
				// The project keeps naming it (validation and export report it) until another is set.
				if (removesStartScene)
					warnings.push_back("this was the project's start scene; set another with project.set startScene");
				if (removesIcon)
					warnings.push_back("this was the project's icon; set another with project.set icon (\"\" for none)");
				// Instances of a deleted prefab keep their state; each one is reported.
				AddPrefabMerges(result, warnings, session.OnFileWritten(path));
				if (!warnings.empty())
					result["warnings"] = std::move(warnings);
				return result; }, CommandPolicy::EditBoundary);

			Add(registry, "asset.list", "Lists project asset files, optionally filtered by extensions (e.g. ['.lua']). An incomplete scan lists what it found, with the reason in 'warnings'. Entries with names that are not valid Unicode have no key: they are left out and listed in 'warnings' (files of the requested types, and every such folder, since it may hold any type).", Schema::Object({ { "extensions", Schema::Array("array of strings", Schema::String()) } }), [](AutomationSession&, const json& params) {
				RequireProject();
				// The schema has checked the types.
				std::vector<std::string> extensions;
				if (auto it = params.find("extensions"); it != params.end())
					extensions = it->get<std::vector<std::string>>();
				std::string error;
				std::set<std::string> skipped;
				json result = { { "assets", AssetManager::ListAssets(extensions, &error, &skipped) } };
				json warnings = json::array();
				if (!error.empty())
					warnings.push_back(error);
				AddSkippedEntryWarnings(skipped, warnings);
				if (!warnings.empty())
					result["warnings"] = std::move(warnings);
				return result;
			});

			Add(registry, "asset.compress_texture", "Compresses an 8-bit PNG/JPEG/TGA/BMP source to a separate KTX2/BC7 asset with mipmaps. Keeps the source, caches by source contents and sRGB setting in Cache/Textures. Reference the output asset to use compression; HDR sources are rejected.", Schema::Object({ { "path", Schema::String("source image under Assets/", true) }, { "output", Schema::String("separate output .ktx2 under Assets/", true) }, { "srgb", Schema::Boolean("sRGB mip filtering, encoder weights and format tag (default true); false for normal/data maps") } }, { "path", "output" }), [](AutomationSession& session, const json& params) {
				auto project = RequireProject();
				const auto path = ProjectRelative(RequireString(params, "path"));
				const auto output = ProjectRelative(RequireString(params, "output"));
				for (const std::string& name : { path, output })
					RequireAssetsPath(*project, name, "texture import");
				if (FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(output)) != ".ktx2")
					throw CommandError("compressed output must have a .ktx2 extension: " + output);
				const auto outputPath = Project::ResolvePath(output);
				const bool overwrites = FileSystem::Exists(outputPath);
				const bool adoptsOrphan = !overwrites && FileSystem::Exists(Project::ResolvePath(output + ".meta"));

				json warnings = json::array();
				// The cache only saves re-encoding, so a linked cache folder (which could redirect writes outside the
				// project) disables it instead of failing the command.
				const std::filesystem::path cacheRelative = "Cache/Textures";
				std::filesystem::path cacheDirectory = project->GetDirectory() / cacheRelative;
				std::filesystem::path cacheEntry;
				std::error_code cacheError;
				if (FileSystem::FindSymlink(project->GetDirectory(), cacheRelative, cacheEntry, cacheError))
				{
					const auto entry = FileSystem::PathToUtf8(cacheEntry);
					warnings.push_back("compressed without the cache: " + (cacheError ? "cannot inspect " + entry + ": " + FileSystem::DescribeError(cacheError) : entry + " is a symlink"));
					BR_CORE_WARN("asset.compress_texture: {}", warnings.back().get<std::string>());
					cacheDirectory.clear();
				}

				std::string error;
				TextureCompression::ImportResult import;
				if (!TextureCompression::Import(Project::ResolvePath(path), outputPath, cacheDirectory, OptionalBool(params, "srgb", true), import, error))
					throw CommandError(error);
				for (const auto& warning : import.Warnings)
					warnings.push_back(warning);
				const auto report = session.OnFileWritten(output);
				json result = { { "path", output }, { "source", path }, { "cacheHit", import.CacheHit }, { "overwrote", overwrites }, { "reloaded", report.Assets } };
				if (adoptsOrphan)
				{
					warnings.push_back("output adopted existing orphan metadata; its previous UUID is retained: " + output);
					BR_CORE_WARN("asset.compress_texture: {}", warnings.back().get<std::string>());
				}
				if (!warnings.empty())
					result["warnings"] = std::move(warnings);
				return result;
			});

			Add(registry, "asset.write", "Writes a text asset (script, prefab, scene, animation graph, mask or joint map, material...) to a project-relative path and reloads it. A .banimgraph, .banimmask, .bjointmap or .bmaterial is validated before replacing the file; a material's texture problems (missing, wrong letter case, unresolved, not a texture, outside Assets/, an embedded image the model lacks) are reported in 'warnings', as project.validate reports them. A script written while the game runs (Play, not Simulate) is hot reloaded (see 'scripts'). A .bprefab merges into the instances that read it in the edit scene and a waiting scene ('prefabInstances'); what reading it finds, instances that cannot merge and what those merges find are 'warnings'.", Schema::Object({ { "path", Schema::String("string", true) }, { "content", Schema::String("string") } }, { "path", "content" }), [](AutomationSession& session, const json& params) {
				RequireProject();
				const std::string path = ProjectRelative(RequireString(params, "path"));
				const json& content = Require(params, "content");
				if (!content.is_string())
					throw CommandError("'content' must be a string");
				const std::string& text = content.get_ref<const std::string&>();
				std::string parseError;
				if (AnimationGraph::IsGraphFile(path) && !AnimationGraph::FromText(text, parseError))
					throw CommandError("invalid animation graph: " + parseError);
				if (AnimationMask::IsMaskFile(path) && !AnimationMask::FromText(text, parseError))
					throw CommandError("invalid animation mask: " + parseError);
				if (AnimationJointMap::IsJointMapFile(path) && !AnimationJointMap::FromText(text, parseError))
					throw CommandError("invalid animation joint map: " + parseError);
				std::vector<std::string> warnings;
				if (MaterialAsset::IsMaterialFile(path))
				{
					std::string error;
					// Parse warnings (unresolved UUIDs) are covered by AddMaterialTextureWarnings, with the same wording.
					const auto material = MaterialAsset::FromText(text, error);
					if (!material)
						throw CommandError("invalid material: " + error);
					AddMaterialTextureWarnings(*material, warnings);
				}
				// Prefabs are not rejected here, as instances report what stops them merging, but what no instance reports must
				// not go unsaid.
				if (FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(path)) == ".bprefab")
				{
					json entities;
					std::string error;
					Prefab::ReadReport prefab;
					if (Prefab::ReadText(text, Project::ResolvePath(path), entities, error, &prefab))
						warnings.insert(warnings.end(), prefab.Warnings.begin(), prefab.Warnings.end());
					else
						warnings.push_back(error);
				}
				const bool adoptsOrphan = !FileSystem::Exists(Project::ResolvePath(path)) && FileSystem::Exists(Project::ResolvePath(path + ".meta"));
				if (!FileSystem::WriteTextFile(Project::ResolvePath(path), content.get<std::string>()))
					throw CommandError("cannot write '" + path + "'");
				const AssetReloadReport report = session.OnFileWritten(path);
				json result = { { "path", path }, { "bytes", content.get<std::string>().size() }, { "scripts", ScriptReloadJson(report) } };
				AddPrefabMerges(result, warnings, report);
				if (adoptsOrphan)
				{
					auto warning = "restored source next to existing metadata; its previous UUID is retained: " + path;
					BR_CORE_WARN("{}", warning);
					warnings.push_back(std::move(warning));
				}
				if (!warnings.empty())
					result["warnings"] = warnings;
				return result;
			});

			Add(registry, "asset.refresh", "Reloads the asset files changed on disk since changes were last reported (by an earlier refresh, by the editor's automatic reload, or since the project was opened in this process): models, textures, animation graphs and material files load again on their next use (graphs restart from their initial state and parameter defaults), and while the game runs (Play, not Simulate) changed scripts are hot reloaded (instances keep their fields; OnReload is called). With 'paths', reloads those files instead, changed or not (a path that does not exist and was not known is an error, and nothing is reloaded then). Files the session wrote itself (asset.write, scene.save, ...) are not reported again. Changed .bprefab files merge into the edit scene's instances ('prefabInstances'; instances that cannot merge are 'warnings'). Without 'paths', fails when Assets/ cannot be read, and lists entries skipped for names that are not valid Unicode in 'warnings'.", Schema::Object({ { "paths", Schema::Array("array of project-relative file paths, optional", Schema::String("string", true)) } }), [](AutomationSession& session, const json& params) {
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
							throw CommandError("cannot read '" + path + "': " + FileSystem::DescribeError(error));
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
					AddSkippedEntryWarnings(session.GetSkippedEntries(), warnings);
				}
				const AssetReloadReport report = session.ReloadAssets(changes);
				json changed = json::array();
				for (const FileChange& change : report.Changed)
					changed.push_back({ { "path", change.Path }, { "type", FileChangeTypeName(change.Type) } });
				json result = { { "changed", changed }, { "reloaded", report.Assets }, { "scripts", ScriptReloadJson(report) } };
				AddPrefabMerges(result, warnings, report);
				if (!warnings.empty())
					result["warnings"] = std::move(warnings);
				return result;
			});

			Add(registry, "asset.preload", "Requests mesh/texture CPU loads on the shared worker pool. A .bmaterial is read at once and its textures are requested too (each reported as its own path after the requested ones; one whose UUID does not resolve is failed). Returns a status (pending, ready or failed) and error for each path. Poll by repeating in a persistent serve/mcp/editor session; use wait=true for one-shot CLI calls. wait=true joins these requests, including embedded image dependencies. Reload/refresh retries failed loads. GPU upload happens in the renderer.", Schema::Object({ { "paths", Schema::Array("array of mesh/texture/.bmaterial paths, Model.glb#image/N keys or built-in mesh keys", Schema::String("string", true)) }, { "wait", Schema::Boolean("wait for these loads, default false") } }, { "paths" }), [](AutomationSession&, const json& params) {
				const auto& paths = params.at("paths");
				if (paths.size() > 4096)
					throw CommandError("at most 4096 preload paths are allowed");
				// Validated requests retain their published status while the whole batch is scheduled.
				enum class Kind
				{
					Mesh,
					Texture,
					// A material file, parsed before anything is scheduled; its textures follow as requests of their own.
					Material,
					// A material's texture whose UUID does not resolve: nothing to load, reported as failed.
					Unresolved
				};
				struct PreloadRequest
				{
					std::string Key;
					Kind Type = Kind::Texture;
					bool Ready = false;
				};
				std::vector<PreloadRequest> requests;
				// Validate the entire batch before creating work.
				for (const auto& value : paths)
				{
					const std::string raw = value.get<std::string>();
					const std::string key = Primitives::IsBuiltinKey(raw) ? raw : ProjectRelative(raw);
					Kind kind = Kind::Texture;
					if (Primitives::IsBuiltinKey(key) || AssetManager::IsMeshFile(key))
						kind = Kind::Mesh;
					else if (MaterialAsset::IsMaterialFile(key))
						kind = Kind::Material;
					// A malformed embedded image index fails at load, with the loader's message.
					else if (!AssetManager::IsTextureFile(key) && !(GetSubAssetFragment(key).starts_with(EmbeddedImageMarker) && AssetManager::IsMeshFile(std::string(GetAssetSource(key)))))
						throw CommandError("unsupported preload asset '" + key + "'");
					requests.push_back({ key, kind });
				}
				// Material files are small and parsed here; what takes time is their textures.
				std::unordered_set<std::string> requested;
				for (const auto& request : requests)
					requested.insert(request.Key);
				const size_t requestedCount = requests.size();
				for (size_t i = 0; i < requestedCount; i++)
				{
					if (requests[i].Type != Kind::Material)
						continue;
					const auto material = AssetManager::GetMaterial(requests[i].Key);
					requests[i].Ready = material != nullptr;
					if (material)
						for (const std::string& texture : MaterialAsset::GetTextures(*material))
							if (requested.insert(texture).second)
								requests.push_back({ texture, AssetRegistry::IsUnresolvedKey(texture) ? Kind::Unresolved : Kind::Texture });
				}
				AssetManager::PublishCompleted();
				const bool wait = OptionalBool(params, "wait", false);
				// Schedule every path before any blocking getter can join an import.
				for (auto& request : requests)
					if (request.Type == Kind::Mesh)
						request.Ready = static_cast<bool>(AssetManager::RequestMesh(request.Key));
					else if (request.Type == Kind::Texture)
						request.Ready = static_cast<bool>(AssetManager::RequestTexture(request.Key));
				if (wait)
				{
					// Activate deferred image batches as their parents finish, before joining textures.
					std::unordered_set<std::string> joinedParents;
					for (const auto& request : requests)
					{
						if (request.Type != Kind::Texture)
							continue;
						const auto source = AssetManager::GetSourceFile(request.Key);
						if (source != request.Key && joinedParents.insert(source).second)
						{
							AssetManager::GetMesh(source);
							AssetManager::PublishCompleted();
						}
					}
				}
				json result = json::array();
				for (const auto& request : requests)
				{
					const auto& key = request.Key;
					bool ready = request.Ready;
					if (wait && request.Type == Kind::Mesh)
						ready = static_cast<bool>(AssetManager::GetMesh(key));
					else if (wait && request.Type == Kind::Texture)
						ready = static_cast<bool>(AssetManager::GetTexture(key));
					const char* status = "failed";
					if (ready)
						status = "ready";
					else if (AssetManager::IsLoading(key))
						status = "pending";
					const std::string error = request.Type == Kind::Unresolved ? RequireProject()->GetAssets().DescribeUnresolved(key) : AssetManager::GetError(key);
					result.push_back({ { "path", key }, { "status", status }, { "error", error } });
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

			Add(registry, "prefab.open", "Opens a prefab for editing in its own scene, where its outermost nested instances (and a variant's root) are ordinary instances with their overrides; 'warnings' says what reading it found (see prefab.info) and which nested instances could not be tracked. The current scene, with its unsaved edits, selection and undo history (in the editor), waits until prefab.close. While the prefab is open, every edit-boundary command except prefab.save and prefab.close is rejected, and so is project.export (today: scene.new/open/save, project.create/open, asset.move/rename/delete/resolve, play.start, replay.run, prefab.open/apply)", Schema::Object({ { "path", Schema::String(".bprefab path under Assets/", true) } }, { "path" }), [](AutomationSession& session, const json& params) {
				const auto path = RequirePrefabPath(params);
				std::string error;
				std::vector<std::string> warnings;
				if (!session.OpenPrefab(path, error, warnings))
					throw CommandError(error);
				return json{ { "path", path }, { "entities", session.GetEditScene()->GetEntityCount() }, { "warnings", warnings } }; }, CommandPolicy::EditBoundary);
			Add(registry, "prefab.save", "Saves the open prefab and merges it into the instances of the waiting scene, keeping their overrides ('instances' counts those that merged). Instances that cannot merge are listed in 'warnings' (a rejected one keeps its previous state; a partly merged one merges fully on prefab.refresh), with what reading found (see prefab.info), entities its merges moved, and a variant root's move, which is not saved. Instances in the open prefab are saved as nested instances, holding only their overrides, after merging what their prefabs changed meanwhile, and a root that is an instance makes it a variant of that prefab. Rejected when the file changed on disk since it was opened or last saved (unless force), when the scene does not have exactly one root, when an instance in it holds this prefab or cannot read or merge its own (nothing merges when one cannot be read), or when the result could not be read back (too deep, too large).", Schema::Object({ { "force", Schema::Boolean("overwrite the file even if it changed on disk (default false)") } }), [](AutomationSession& session, const json& params) {
				std::string error;
				AssetReloadReport report;
				std::vector<std::string> warnings;
				if (!session.SavePrefab(OptionalBool(params, "force", false), error, report, warnings))
					throw CommandError(error);
				return PrefabWriteResult(session.GetPrefabPath(), report, std::move(warnings)); }, CommandPolicy::EditBoundary);
			Add(registry, "prefab.close", "Returns to the scene that waited during prefab editing, with its path, unsaved state and selection. Unsaved prefab edits require discard=true.", Schema::Object({ { "discard", Schema::Boolean("discard unsaved prefab edits (default false)") } }), [](AutomationSession& session, const json& params) {
				std::string error;
				if (!session.ClosePrefab(OptionalBool(params, "discard", false), error))
					throw CommandError(error);
				return json{ { "path", session.GetScenePath() } }; }, CommandPolicy::EditBoundary);
			const auto entityOnly = Schema::Object({ { "entity", Schema::Entity("prefab instance root") } }, { "entity" });
			Add(registry, "prefab.apply", "Writes an instance's overrides into its prefab file, then merges the file into the instances of the open scene ('instances'). The root's position and rotation, entities the instance added and references to entities outside the prefab stay local ('warnings' names such references, and what the file's prefabs report). An untracked instance is adopted first, so all its differences are applied. A nested instance's overrides go into the prefab holding it, and the root of a nested instance is rejected: use the instance holding it. Edit mode only.", entityOnly, [](AutomationSession& session, const json& params) {
				Entity root = RequirePrefabInstance(session, params);
				const auto path = root.GetComponent<PrefabComponent>().Prefab;
				RequireAssetsPath(*RequireProject(), path, "prefab");
				std::string error;
				std::vector<std::string> warnings;
				if (!Prefab::Apply(root, Project::ResolvePath(path), warnings, error))
					throw CommandError(fmt::format("instance '{}' of {}: {}", root.GetName(), path, error));
				MarkEdited(session);
				return PrefabWriteResult(path, session.OnFileWritten(path), std::move(warnings)); }, CommandPolicy::EditBoundary);
			Add(registry, "prefab.refresh", "Merges the prefab file into an instance, keeping its overrides, entity IDs and the entities it added. Prefab writes and scene.open merge tracked instances automatically; this catches up an instance by hand. Instances without tracking (saved before it existed, or a nested root copied out of its instance) are adopted first (here, by revert and apply, and by the merge after a write of their own prefab; scene.open and project.open leave them untracked): their differences from the current file become overrides, so that file's changes are not taken. 'warnings', when present, says what reading the prefab found and which of the instance's entities moved. Edit mode only.", entityOnly, [](AutomationSession& session, const json& params) {
				Entity root = RequirePrefabInstance(session, params);
				return MergeFromSourceFile(session, root, Prefab::RefreshMode::Merge); });
			Add(registry, "prefab.revert", "Discards an instance's overrides: all of them, or the one override path 'field' from prefab.overrides. The root's position, rotation and parent and the entities the instance added stay. An untracked instance is adopted first, so reverting all discards its differences from the prefab. 'warnings', when present, says what reading the prefab found and which of the instance's entities moved. Edit mode only.", Schema::Object({ { "entity", Schema::Entity("prefab instance root") }, { "field", Schema::String("one override path from prefab.overrides, optional") } }, { "entity" }), [](AutomationSession& session, const json& params) {
				Entity root = RequirePrefabInstance(session, params);
				return MergeFromSourceFile(session, root, Prefab::RefreshMode::RevertAll, OptionalString(params, "field")); });
			Add(registry, "prefab.unpack", "Removes an instance's prefab link and keeps its entities as ordinary scene entities. It first catches up with its prefab. Nested instances stay instances of their prefabs, tracked from now on, each with its own overrides and with the IDs they had ('warnings' names any whose prefab cannot be read). On the root of a nested instance inside an instance, it is an override of that instance. Edit mode only.", entityOnly, [](AutomationSession& session, const json& params) {
				Entity root = RequireEditablePrefabRoot(session, params);
				std::vector<std::string> warnings;
				Prefab::Unpack(root, warnings);
				MarkEdited(session);
				json result = EntitySummary(root);
				if (!warnings.empty())
					result["warnings"] = std::move(warnings);
				return result; });
			Add(registry, "prefab.overrides", "Lists an instance's overrides: JSON-pointer paths into its source records ('/<source ID>' for a whole entity: null when the instance deleted it, its record when the instance kept an entity the prefab no longer has; '/<source ID>/Name', '/<source ID>/Parent', '/<source ID>/Components/<component>[/<field>...]') with the instance's value, null where it removed something. Entity references are source IDs for the instance's entities, scene UUIDs otherwise, and {\"External\": <UUID>} where a scene UUID equals a source ID. Fields are whole values (vectors and lists are not merged element by element); the root's position and rotation are placement, not overrides. 'members' maps source entity IDs to instance entity IDs (including reserved IDs of entities the instance deleted); 'tracked' is false for an instance saved before tracking, copied out of an instance (it has no overrides until adopted) or created during Play, and for a nested root. A nested instance's entities are part of the instance holding it, whose overrides list them; on the root of a nested instance, 'enclosing' names that instance.", entityOnly, [](AutomationSession& session, const json& params) { return PrefabOverridesResult(RequirePrefabRoot(session, params)); });

			Add(registry, "prefab.create", "Saves an entity tree as a prefab file and makes the tree an instance of it ('linked'; not during Play, while a prefab is open, or when it belongs to another instance or prefab, kept with a warning). Prefab instances inside the tree stay instances: the file holds a nested record with their overrides, so their prefabs' edits keep reaching it. With variant, an instance root becomes a variant of its prefab: the file holds only the instance's differences, without its root's position and rotation, and the instance stays where it is, tracking the variant. What an instance in or around the tree missed from its prefab is merged first (a running game cannot: 'warnings' says so, and its old values are saved as overrides). Then merges the file into the open scene's instances ('instances'). Overwriting a prefab keeps its entity IDs (through the tree's instance link, or by matching names), so existing instances keep merging. Rejected when a nested instance holds this file or cannot read or merge its prefab (nothing merges when one cannot be read), when the result could not be read back (too deep, too large), when saving an instance over a file that cannot be read, or, with variant, when the entity is not a top-level instance root or path is its own prefab.", Schema::Object({ { "entity", Schema::Entity("ID or name") }, { "path", Schema::String(".bprefab path under Assets/, e.g. 'Assets/Prefabs/Enemy.bprefab'", true) }, { "variant", Schema::Boolean("save an instance root as a variant of its prefab (default false)") } }, { "entity", "path" }), [](AutomationSession& session, const json& params) {
				Entity entity = RequireEntity(session, params);
				const std::string path = RequirePrefabPath(params);
				const bool variant = OptionalBool(params, "variant", false);
				const auto* requestedLink = entity.TryGetComponent<PrefabComponent>();
				if (variant && (!requestedLink || Prefab::EnclosingInstance(entity)))
					throw CommandError(fmt::format("'{}' is not a top-level instance root, so it has no prefab to make a variant of", entity.GetName()));
				if (variant && Prefab::SameFile(requestedLink->Prefab, path))
					throw CommandError(fmt::format("{} cannot be a variant of itself", path));
				std::string error;
				std::vector<std::string> warnings;
				const PrefabFileRoot requested = variant ? PrefabFileRoot::Variant : PrefabFileRoot::Plain;
				// What a nested instance has not merged yet would otherwise save as overrides of what its prefab says now.
				// That can add and remove entities, so everything about the tree is looked at afterwards.
				bool merged = false;
				const bool current = Prefab::MergeOutdatedNested(entity, path, requested, merged, warnings, error);
				if (merged)
					MarkEdited(session);
				if (!current)
					throw CommandError(error);
				if (!entity.IsValid())
					throw CommandError("the entity left its instance when that merged its prefab's changes");
				std::vector<Entity> tree;
				CollectTree(entity, tree);
				// Linking would take the tree away from an instance it belongs to, or replace its link to another prefab.
				const Entity enclosing = Prefab::EnclosingInstance(entity);
				if (enclosing)
					warnings.push_back(fmt::format("'{}' stays part of the prefab instance '{}' (not linked to {})", entity.GetName(), enclosing.GetName(), path));
				const std::string existingPrefab = entity.HasComponent<PrefabComponent>() ? entity.GetComponent<PrefabComponent>().Prefab : std::string();
				const bool keepsOtherLink = !existingPrefab.empty() && !Prefab::SameFile(existingPrefab, path) && !variant;
				if (keepsOtherLink)
					warnings.push_back(fmt::format("'{}' stays an instance of {} (not linked to {})", entity.GetName(), existingPrefab, path));
				json entities = SceneSerializer::SerializeEntityTree(entity);
				const auto rootKind = Prefab::AssignSourceIds(entity, path, entities, requested, warnings, error);
				if (!rootKind)
					throw CommandError(error);
				if (!SceneSerializer::SavePrefabEntities(entities, Project::ResolvePath(path), error, *rootKind, &warnings))
					throw CommandError(error);
				// The saved tree becomes an instance of its prefab, so saving it again keeps every entity's ID even after
				// renames and moves. Linked before the write merges, so the tree itself has nothing to merge. A variant relinks
				// on purpose: it replaces the link to its base.
				const bool link = !enclosing && !keepsOtherLink && !session.IsPlaying() && !session.IsEditingPrefab();
				if (variant && !link)
					warnings.push_back(fmt::format("'{}' stays an instance of {} while the game runs or a prefab is open (not linked to {})", entity.GetName(), existingPrefab, path));
				if (link)
				{
					Prefab::Link(tree, entities, path);
					MarkEdited(session);
				}
				json result = PrefabWriteResult(path, session.OnFileWritten(path), std::move(warnings));
				result["linked"] = link;
				return result;
			});
			Add(registry, "prefab.variant", "Creates a prefab that inherits another: its root is an instance of 'base' without any override, so everything follows the base until the variant changes it. Edit it like any prefab (prefab.open, prefab.apply): the file holds only its differences, and saves never write its root's position or rotation, so those follow the base. 'path' defaults to '<base> Variant.bprefab' beside the base, numbered when taken, and the result names it. Rejected when 'path' exists, or when the base cannot be read or the result could not be read back (too deep).", Schema::Object({ { "base", Schema::String(".bprefab path of the prefab to inherit", true) }, { "path", Schema::String(".bprefab path of the new variant, under Assets/, optional") } }, { "base" }), [](AutomationSession& session, const json& params) {
				const std::string base = RequirePrefabPath(params, "base");
				const auto taken = [](const std::string& candidate) {
					std::error_code error;
					if (std::filesystem::exists(Project::ResolvePath(candidate), error))
						return true;
					if (error)
						throw CommandError("cannot check '" + candidate + "': " + FileSystem::DescribeError(error));
					return false;
				};
				std::string path;
				if (params.contains("path"))
				{
					path = RequirePrefabPath(params);
					if (taken(path))
						throw CommandError(path + " already exists");
				}
				else
				{
					const std::filesystem::path source = FileSystem::PathFromUtf8(base);
					const std::string stem = FileSystem::PathToGenericUtf8(source.parent_path() / source.stem()) + " Variant";
					path = stem + ".bprefab";
					for (int number = 2; taken(path); ++number)
						path = fmt::format("{} {}.bprefab", stem, number);
				}
				std::string error;
				std::vector<std::string> warnings;
				if (!Prefab::CreateVariant(base, Project::ResolvePath(path), warnings, error))
					throw CommandError(error);
				return PrefabWriteResult(path, session.OnFileWritten(path), std::move(warnings));
			});
			Add(registry, "prefab.info", "Describes a prefab file without opening it: 'entities' (after expanding nested instances), 'base' (the prefab a variant inherits, else null), 'nested' (the nested instances this file holds directly, as { entity, prefab }, the variant's root included), 'dependencies' (every prefab it reads, directly or not; a change to any of them reaches its tracked instances) and 'warnings' (overrides of entities their prefab no longer has, entities moved to the root because their parent is gone, fields a nested record ignores, records replacing a nested prefab's, and Prefab components ignored in a file too old to nest).", Schema::Object({ { "path", Schema::String(".bprefab path under Assets/", true) } }, { "path" }), [](AutomationSession&, const json& params) {
				const std::string path = RequirePrefabPath(params);
				json entities;
				std::string error;
				Prefab::ReadReport report;
				const auto absolute = Project::ResolvePath(path);
				if (!Prefab::ReadFile(absolute, entities, error, &report))
					throw CommandError(error);
				json nested = json::array();
				for (const auto& instance : report.Nested)
					nested.push_back({ { "entity", instance.Id }, { "prefab", instance.Prefab } });
				report.Files.erase(Prefab::FileKey(absolute));
				json dependencies = json::array();
				for (const auto& [key, file] : report.Files)
					dependencies.push_back(file);
				return json{ { "path", path }, { "entities", entities.size() }, { "base", report.Base.empty() ? json(nullptr) : json(report.Base) }, { "nested", std::move(nested) }, { "dependencies", std::move(dependencies) }, { "warnings", report.Warnings } };
			});

			Add(registry, "prefab.instantiate", "Instantiates a prefab into the scene, with the prefabs it nests or is based on. The prefab file needs unique nonzero entity IDs and one root listed first. The root's Prefab component tracks the instance (with a baseline in edit mode), so prefab saves merge into it; 'warnings', when present, says what reading the file found (see prefab.info).", Schema::Object({ { "path", Schema::String(".bprefab path, e.g. 'Assets/Prefabs/Enemy.bprefab'", true) }, { "parent", Schema::Nullable(Schema::Entity("ID or name, optional")) }, { "position", Schema::Vector("[x, y, z], optional", 3) } }, { "path" }), [](AutomationSession& session, const json& params) {
				Scene& scene = RequireScene(session);
				const std::string path = ProjectRelative(RequireString(params, "path"));
				Entity parent = params.contains("parent") && !params["parent"].is_null() ? ResolveEntity(scene, params["parent"], "parent") : Entity{};
				std::string error;
				std::vector<std::string> warnings;
				Entity root = SceneSerializer::InstantiatePrefab(scene, Project::ResolvePath(path), path, parent, error, &warnings);
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
				json result = EntitySummary(root);
				if (!warnings.empty())
					result["warnings"] = std::move(warnings);
				return result;
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
				throw CommandError("reference image not found: '" + FileSystem::PathToUtf8(referencePath) + "'");
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
						BR_CORE_WARN("image.compare: cannot remove the old diff image '{}': {}", FileSystem::PathToUtf8(diffPath), FileSystem::DescribeError(removeError));
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

		float NormalizedAnimationTime(const json& params)
		{
			float time = 0;
			if (params.contains("time") && (!JsonToFloat(params.at("time"), time) || time < 0 || time > 1))
				throw CommandError("time must be normalized in [0, 1]");
			return time;
		}
		void RegisterAnimationAuthoring(CommandRegistry& registry)
		{
			Add(registry, "animation.control", "Controls a PropertyAnimation in Play or Simulate. action is Play (restart at zero), Pause, Resume, Stop (sample zero), Seek (sample normalized time), or Status. Returns {time, playing, failed, error}; time is normalized. Pause/Resume on a failed timeline fail until Play/Stop/Seek succeeds or its component is edited. Component edits restart clocks using PlayOnStart. A failed sample may have applied earlier components.", Schema::Object({ { "entity", Schema::Entity("ID or name") }, { "action", Schema::String("Play/Pause/Resume/Stop/Seek/Status", true) }, { "time", Schema::Number("normalized time for Seek, default 0", 0, 1) } }, { "entity", "action" }), [](AutomationSession& session, const json& params) {
				Entity entity = RequireEntity(session, params);
				auto* system = entity.GetScene()->GetPropertyAnimationSystem();
				if (!system || !entity.HasComponent<PropertyAnimationComponent>())
					throw CommandError("requires PropertyAnimation in Play or Simulate");
				const auto action = RequireString(params, "action");
				std::string error;
				if (action != "Status" && !system->Control(entity, action, NormalizedAnimationTime(params), error))
					throw CommandError(error);
				return system->Status(entity);
			});
			Add(registry, "animation.key", "Captures an existing component field of this entity, or of `target`, into this entity's PropertyAnimation timeline at normalized time [0,1]: a number (up to 4 lanes), integer, boolean, string, enum or asset reference, top-level or inside list elements picked by index or name (\"Layers[Upper].Weight\"). Creates the component/track as needed (Stepped for integer, boolean, string, enum and asset fields, which always step), inserts sorted keys and replaces an exact-time key. Unused Value lanes are zero; strings and enums go in Text, asset references in Asset. All writes use ComponentRegistry.", Schema::Object({ { "entity", Schema::Entity("ID or name of the entity holding the timeline") }, { "component", Schema::String("component name", true) }, { "field", Schema::String("field or path, e.g. Translation or Layers[Upper].Weight", true) }, { "time", Schema::Number("normalized time", 0, 1) }, { "target", Schema::Entity("ID or name of the entity animated, default the timeline's own") } }, { "entity", "component", "field", "time" }), [](AutomationSession& session, const json& params) {
				Entity entity = RequireEntity(session, params);
				std::string error;
				const UUID target = params.contains("target") ? RequireEntity(session, params, "target").GetUUID() : UUID(0);
				if (!PropertyAnimation::Key(entity, RequireString(params, "component"), RequireString(params, "field"), NormalizedAnimationTime(params), error, target))
					throw CommandError(error);
				MarkEdited(session);
				return ComponentRegistry::Find("PropertyAnimation")->Serialize(entity);
			});
			Add(registry, "animation.sample", "Scrubs this entity's PropertyAnimation at normalized time [0,1] through the component registry. Modifies the current scene; edit-mode samples are undoable. Missing targets or invalid sampled values fail; components applied before a failure stay changed. Does not move the runtime clock.", Schema::Object({ { "entity", Schema::Entity("ID or name") }, { "time", Schema::Number("normalized time", 0, 1) } }, { "entity", "time" }), [](AutomationSession& session, const json& params) {
				Entity entity = RequireEntity(session, params);
				if (!entity.HasComponent<PropertyAnimationComponent>())
					throw CommandError("entity has no PropertyAnimation");
				std::string error;
				MarkEdited(session);
				if (!PropertyAnimation::Sample(entity, entity.GetComponent<PropertyAnimationComponent>(), NormalizedAnimationTime(params), error))
					throw CommandError(error);
				return json::object();
			});
		}

		void RegisterMaterials(CommandRegistry& registry)
		{
			Add(registry, "material.create", "Writes a material file (.bmaterial under Assets/) with UUID texture references: with 'entity', the material that entity's Material component draws (its Material asset when set, failing if that does not load, otherwise its inline values), otherwise the defaults. Returns {path, warnings?} (the texture problems asset.write reports). Refuses to replace an existing file unless overwrite=true. Reference the file from Material.Material.", Schema::Object({ { "path", Schema::String("Assets/...bmaterial", true) }, { "entity", Schema::Nullable(Schema::Entity("ID or name, optional")) }, { "overwrite", Schema::Boolean("replace an existing file, default false") } }, { "path" }), [](AutomationSession& session, const json& params) {
				auto project = RequireProject();
				const std::string path = ProjectRelative(RequireString(params, "path"));
				RequireAssetsPath(*project, path, "material");
				if (!MaterialAsset::IsMaterialFile(path))
					throw CommandError("material files must have a .bmaterial extension: " + path);
				MaterialData material;
				if (params.contains("entity") && !params["entity"].is_null())
				{
					Entity entity = RequireEntity(session, params);
					const auto* component = entity.TryGetComponent<MaterialComponent>();
					if (!component)
						throw CommandError("entity '" + entity.GetName() + "' has no Material component");
					if (component->Material.empty())
						material = MaterialAsset::FromComponent(*component);
					else if (const auto asset = AssetManager::GetMaterial(component->Material))
						material = *asset;
					else
						throw CommandError("cannot read Material '" + component->Material + "': " + AssetManager::GetError(component->Material));
				}
				material.Name.clear();
				RequireNewFile(path, OptionalBool(params, "overwrite", false));
				WriteMaterialText(session, path, MaterialFileText(path, material));
				std::vector<std::string> warnings;
				AddMaterialTextureWarnings(material, warnings);
				json result = { { "path", path } };
				if (!warnings.empty())
					result["warnings"] = warnings;
				return result;
			});

			Add(registry, "material.extract", "Writes one material file per material a glTF model imports, keeping every imported value (alpha mode, sidedness, strengths, per-map UV transforms and sets): <folder>/<material name>.bmaterial (made a safe, unique file name; Material_N when unnamed), folder defaulting to a Materials folder next to the model. Returns {model, materials: [{index, name, path, key}], warnings?} (texture problems per file). Textures reference the model's image files and embedded images (Model.glb#image/N) by UUID. Checks every output before writing: an existing file fails unless overwrite=true. The model's own materials stay addressable as Model.glb#material/N.", Schema::Object({ { "model", Schema::String("Assets/....gltf or .glb", true) }, { "folder", Schema::String("output folder under Assets/, optional") }, { "overwrite", Schema::Boolean("replace existing files, default false") } }, { "model" }), [](AutomationSession& session, const json& params) {
				auto project = RequireProject();
				const std::string model = ProjectRelative(RequireString(params, "model"));
				if (!AssetManager::IsMeshFile(model))
					throw CommandError("'" + model + "' is not a glTF model (.gltf, .glb)");
				const std::string folder = ProjectRelative(OptionalString(params, "folder", FileSystem::PathToGenericUtf8(FileSystem::PathFromUtf8(model).parent_path() / "Materials")));
				RequireAssetsPath(*project, folder, "material output");
				const auto mesh = AssetManager::GetMesh(model);
				if (!mesh)
					throw CommandError("cannot load '" + model + "': " + AssetManager::GetError(model));
				const size_t count = mesh->GetImportedMaterialCount();
				if (count == 0)
					throw CommandError("'" + model + "' has no materials");
				const bool overwrite = OptionalBool(params, "overwrite", false);
				std::vector<std::string> paths;
				paths.reserve(count);
				// Names compare case-insensitively: two materials must not share one file on Windows or macOS.
				std::set<std::string> used;
				for (size_t i = 0; i < count; i++)
				{
					std::string name = MaterialAsset::MakeFileName(mesh->Materials[i].Name);
					if (name.empty())
						name = "Material_" + std::to_string(i);
					std::string unique = name;
					for (int suffix = 2; !used.insert(FileSystem::ToLowerAscii(unique)).second; suffix++)
						unique = name + "_" + std::to_string(suffix);
					std::string path = folder;
					path.append("/").append(unique).append(MaterialAsset::Extension);
					RequireNewFile(path, overwrite, "; nothing was written");
					paths.push_back(std::move(path));
				}
				// Every file's text is made first: only a file system error can stop the writes part way.
				std::vector<std::string> texts;
				texts.reserve(count);
				for (size_t i = 0; i < count; i++)
					texts.push_back(MaterialFileText(paths[i], mesh->Materials[i]));
				json materials = json::array();
				json warnings = json::array();
				for (size_t i = 0; i < count; i++)
				{
					WriteMaterialText(session, paths[i], texts[i], fmt::format(" ({} of {} files were written before it)", i, count));
					std::vector<std::string> fileWarnings;
					AddMaterialTextureWarnings(mesh->Materials[i], fileWarnings);
					for (const auto& warning : fileWarnings)
						warnings.push_back(paths[i] + ": " + warning);
					materials.push_back({ { "index", i }, { "name", mesh->Materials[i].Name }, { "path", paths[i] }, { "key", MakeSubAssetKey(model, ImportedMaterialMarker, static_cast<uint32_t>(i)) } });
				}
				json result = { { "model", model }, { "materials", materials } };
				if (!warnings.empty())
					result["warnings"] = warnings;
				return result;
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

			Add(registry, "play.step", "Advances play mode by N fixed frames (default dt 1/60), with every in-process network client (net.connect), and reports script errors (clientScriptErrors: those of in-process clients, [{client, errors, ended}], ended for a client that ended since the last step, present when there are some) and the playing scene (which Lua Scene.Load may switch).", Schema::Object({ { "frames", Schema::Nullable(Schema::Integer("integer (default 1)", 1, 100000)) }, { "dt", json{ { "type", "number" }, { "description", "number, seconds per frame" }, { "exclusiveMinimum", 0.0 }, { "maximum", 0.25 } } }, { "assertNoErrors", Schema::Nullable(Schema::Boolean("bool: fail the command if any script error occurred")) }, { "hash", Schema::Nullable(Schema::Boolean("bool: also return stateHash (see scene.hash)")) }, { "expectHash", Schema::String("string: fail unless the state hash after the step (which has run either way) equals this one; also returns stateHash", true) } }), [](AutomationSession& session, const json& params) {
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
				// In-process network clients' scripts fail as the play scene's do.
				json clientErrors = json::array();
				for (auto& [client, ended] : session.TakeEndedClientErrors())
					clientErrors.push_back({ { "client", client }, { "errors", std::move(ended) }, { "ended", true } });
				const std::vector<Ref<Scene>>& clients = session.GetNetworkClients();
				for (size_t i = 0; i < clients.size(); i++)
					if (clients[i] && clients[i]->GetScriptEngine() && !clients[i]->GetScriptEngine()->GetErrors().empty())
						clientErrors.push_back({ { "client", i + 1 }, { "errors", clients[i]->GetScriptEngine()->GetErrors() } });
				if (OptionalBool(params, "assertNoErrors", false) && (!errors.empty() || !clientErrors.empty()))
					throw CommandError("script errors: " + errors.dump(-1, ' ', false, json::error_handler_t::replace) + (clientErrors.empty() ? "" : "; in-process clients: " + clientErrors.dump(-1, ' ', false, json::error_handler_t::replace)));
				json result = { { "time", scene.GetTime() }, { "frame", scene.GetFrameCount() }, { "scriptErrors", errors }, { "quitRequested", scene.IsQuitRequested() }, { "scene", scene.GetName() }, { "scenePath", scene.GetPath() } };
				if (!clientErrors.empty())
					result["clientScriptErrors"] = std::move(clientErrors);
				if (OptionalBool(params, "hash", false) || params.contains("expectHash"))
				{
					const std::string hash = SceneSerializer::ComputeStateHash(scene);
					CheckExpectedHash(params, hash);
					result["stateHash"] = hash;
				}
				return result;
			});

			Add(registry, "replay.record_start", "Starts recording input.key/input.text/input.mouse/input.gamepad commands with the play frame they arrive on. The mouse position and the injected gamepads (with what they hold) are recorded as they are on that frame, and the host's view whenever it changes, checked before every frame and every recorded command: where mouse positions land on the UI canvas, the game resolution scripts read the mouse in, and whether the game has the pointer and the keyboard (routing gates every game input), so replays run the same way in any host. A recording begun before play marks where play started (a playStart event): what came before it applies before the replay starts play, so OnCreate sees the same view and input. Physical gamepads are not recorded (replays run without them): they are listed in physicalGamepads.", Schema::Object(), [](AutomationSession& session, const json&) {
				session.StartRecording();
				json physical = json::array();
				for (uint32_t index = 0; index < Input::MaxGamepads; index++)
				{
					if (Input::GetGamepadSource(index) == GamepadSource::Device)
						physical.push_back(index);
				}
				if (!physical.empty())
					BR_CORE_WARN("replay.record_start: physical gamepads {} are connected; replays run without them, so their input will not replay", physical.dump(-1, ' ', false, json::error_handler_t::replace));
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
				json replay = { { "Format", ReplayFormatName }, { "Version", 1 }, { "Frames", frames }, { "RandomSeed", seed }, { "Events", events } };
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
				// find() on a value that is no object finds nothing; comparing JSON never throws, whatever Format holds.
				const auto format = replay.find("Format");
				if (format == replay.end() || *format != ReplayFormatName)
					throw CommandError("'" + path + "' is not a Breccia replay file" + (format == replay.end() ? std::string() : " ('Format' is " + format->dump() + ")"));
				if (!replay.contains("Events") || !replay["Events"].is_array())
					throw CommandError("'" + path + "' is not a Breccia replay file (no Events)");
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
						else if (command == "input.text")
							ApplyTextInput(eventParams);
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

			Add(registry, "input.text", "Queues one Unicode scalar value for the focused text input this frame. Rejects C0/DEL control characters, surrogates and input beyond the 16384-byte frame buffer. Recorded during replay recording.", Schema::Object({ { "codepoint", Schema::Integer("Unicode scalar value (surrogates rejected)", 0, 0x10ffff) } }, { "codepoint" }), [](AutomationSession& session, const json& params) {
				ApplyTextInput(params);
				session.RecordInput("input.text", params);
				return json{ { "ok", true } };
			});

			Add(registry, "input.mouse", "Sets mouse position, a button state and/or accumulated scroll [x,y] for play mode. Recorded while replay recording is on.", Schema::Object({ { "position", Schema::Vector("[x, y]", 2) }, { "scroll", Schema::Vector("[x, y] wheel delta", 2) }, { "button", Schema::String("string (Left, Right, Middle)", true) }, { "down", Schema::Nullable(Schema::Boolean("bool")) } }), [](AutomationSession& session, const json& params) {
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
					throw CommandError("screenshots need a GPU host (the editor, or 'BrecciaRuntime --frames N --screenshot out.png'); this host is '" + session.HostName + "'");
				const std::string path = RequireString(params, "path");
				const uint32_t width = static_cast<uint32_t>(OptionalInteger(params, "width", 1280, 16, 8192));
				const uint32_t height = static_cast<uint32_t>(OptionalInteger(params, "height", 720, 16, 8192));
				std::string error;
				if (!session.Screenshot(FileSystem::PathFromUtf8(path), width, height, error))
					throw CommandError(error);
				return json{ { "path", path } };
			});

			Add(registry, "memory.stats", "Returns logical retained CPU asset payload bytes, requested GPU buffer bytes, and live Lua heap bytes, with process-lifetime peaks and live allocation counts. Not RSS or driver residency; excludes allocator overhead, GPU textures and render targets. Available headlessly.", Schema::Object(), [](AutomationSession&, const json&) {
				return MemoryTracker::ToJson();
			});

			Add(registry, "render.stats", "Returns the scene renderer's statistics: viewport and render size, render scale and dynamic resolution state, draw, mesh-instance (including transparent meshes, excluding particles), color-pass triangle and culling counts, and the GPU time of each pass averaged over recent frames (editor / GPU hosts only).", Schema::Object(), [](AutomationSession& session, const json&) {
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
					throw CommandError("render.compare needs a GPU host (the editor); this host is '" + session.HostName + "'. Headless, compare a 'BrecciaRuntime --frames N --screenshot' image with image.compare");
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
				const json expected = Asserting([&]() { return ResolveEntityFields(scene, name, params["fields"], entity); });
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
			Add(registry, "events.subscribe", "Starts collecting events: log lines, script errors, play-state changes (play, simulate, stop; pause and resume from the editor's Pause button), collisions (collisionBegin/End and triggerEnter/Exit, in Play and Simulate) and animation events (Animator Events markers playback crossed: name, clip, layer (null for the base), entity, frame; in Play and Simulate). On the editor's TCP server a line-protocol connection receives them as {\"event\": ...} lines without 'ok' (and {\"event\": \"dropped\"} when it falls behind); MCP and CLI clients read them with events.poll. A TCP connection's subscriptions end with it. Returns {subscription, types, minLevel, streamed}.", Schema::Object({ { "types", typesSchema }, { "minLevel", Schema::Nullable(minLevel) } }), [](AutomationSession& session, const json& params) {
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

		json LinkJson(const SimulatedNetwork& network)
		{
			const SimulatedLinkSettings& link = network.GetLinkSettings();
			return { { "latency", link.Latency }, { "jitter", link.Jitter }, { "loss", link.Loss }, { "timeoutSeconds", link.TimeoutSeconds }, { "lost", network.GetLostCount() } };
		}

		// Commands net.command may run on an in-process client: they read a scene or change the playing one, never edit state
		// or play itself (stepping one peer alone would leave the others behind).
		constexpr std::array<const char*, 14> ClientCommands = { "scene.hash", "scene.get", "scene.info", "scene.snapshot", "scene.diff", "entity.get", "entity.list", "entity.transform", "component.get", "component.set", "assert.entity_exists", "assert.component", "lua.exec", "input.actions" };

		json NetTrafficJson(const NetTraffic& traffic)
		{
			return { { "messagesSent", traffic.MessagesSent }, { "messagesReceived", traffic.MessagesReceived }, { "bytesSent", traffic.BytesSent }, { "bytesReceived", traffic.BytesReceived } };
		}

		json NetworkStatusJson(const Scene& scene)
		{
			const NetworkSystem* network = scene.GetNetworkSystem();
			if (!network)
				return { { "role", nullptr } };
			const NetworkStatistics& statistics = network->GetStatistics();
			json connections = json::array();
			for (const NetConnectionInfo& connection : network->GetConnections())
				connections.push_back({ { "peer", connection.PeerId }, { "address", connection.Address }, { "roundTripTime", connection.RoundTripTime }, { "knownEntities", connection.KnownEntities }, { "traffic", NetTrafficJson(connection.Traffic) } });
			return {
				{ "role", NetworkRoleName(network->GetRole()) },
				{ "state", NetConnectionStateName(network->GetState()) },
				{ "peerId", network->GetLocalPeerId() },
				{ "disconnectReason", network->GetDisconnectReason() == NetDisconnectReason::None ? json(nullptr) : json(NetDisconnectReasonName(network->GetDisconnectReason())) },
				{ "port", network->GetRole() == NetworkRole::Host ? json(network->GetListenPort()) : json(nullptr) },
				{ "time", network->GetTime() },
				{ "viewTime", network->GetViewTime() },
				{ "remoteEntities", network->GetRemoteEntityCount() },
				{ "connections", std::move(connections) },
				{ "statistics",
				  { { "traffic", NetTrafficJson(statistics.Traffic) },
					{ "staleMessages", statistics.StaleMessages },
					{ "spawnsApplied", statistics.SpawnsApplied },
					{ "despawnsApplied", statistics.DespawnsApplied },
					{ "updatesApplied", statistics.UpdatesApplied },
					{ "entitiesAdopted", statistics.EntitiesAdopted },
					{ "rejectedValues", statistics.RejectedValues },
					{ "rejectedRecords", statistics.RejectedRecords },
					{ "scriptMessagesSent", statistics.ScriptMessagesSent },
					{ "scriptMessagesReceived", statistics.ScriptMessagesReceived },
					{ "scriptMessagesDropped", statistics.ScriptMessagesDropped },
					{ "inputsApplied", statistics.InputsApplied },
					{ "inputsRejected", statistics.InputsRejected },
					{ "inputsSkipped", statistics.InputsSkipped },
					{ "idleSteps", statistics.IdleSteps },
					{ "corrections", statistics.Corrections } } }
			};
		}

		CommandError NoClientError(size_t index)
		{
			return CommandError(fmt::format("no in-process client {} (net.connect adds one; net.status lists them)", index));
		}

		Ref<Scene> RequireNetworkClient(AutomationSession& session, const json& params)
		{
			const size_t index = static_cast<size_t>(OptionalInteger(params, "client", 0, 1, MaxClientIndex));
			Ref<Scene> client = session.GetNetworkClient(index);
			if (!client)
				throw NoClientError(index);
			return client;
		}

		void RequireNetworkPlay(AutomationSession& session)
		{
			if (!session.IsPlaying() || session.IsSimulating())
				throw CommandError("not playing (use play.start)");
		}

		void RegisterNetwork(CommandRegistry& registry)
		{
			const json clientSchema = Schema::Integer("integer, an in-process client's index (from net.connect)", 1, MaxClientIndex);

			Add(registry, "net.host", "Makes the playing scene the host of a network session. Without udp it listens on the session's in-process network, where net.connect adds clients that play copies of the scene in this process (stepped by play.step, with net.link's latency and loss); with udp, on a real UDP port that BrecciaRuntime --connect, another editor or Lua Network.Connect can join. port 0 picks a free one. Returns {transport, port}.", Schema::Object({ { "udp", Schema::Nullable(Schema::Boolean("bool (default false): a real UDP port instead of the in-process network")) }, { "port", Schema::Nullable(Schema::Integer("integer in [0, 65535] (default 0: a free port)", 0, 65535)) } }), [](AutomationSession& session, const json& params) {
				RequireNetworkPlay(session);
				const bool udp = OptionalBool(params, "udp", false);
				std::string error;
				if (!session.HostNetwork(udp, static_cast<uint16_t>(OptionalInteger(params, "port", 0, 0, 65535)), error))
					throw CommandError(error);
				return json{ { "transport", udp ? "udp" : "simulated" }, { "port", session.GetScene()->GetNetworkSystem()->GetListenPort() } };
			});

			Add(registry, "net.connect", fmt::format("Without address: adds an in-process client to the playing scene's in-process host (net.host): a copy of the edit scene, playing as a client, stepped with the host by play.step (at most {}; a client's Game.Quit ends it). Returns {{client}}, its index for net.status, net.disconnect, net.focus, net.command and scene.hash. With address (host or host:port, the port defaulting to the project's Network.Port): the playing scene joins that UDP host as a client. Returns {{address, port}}. Follow the connection with net.status (state Connecting, then Connected or Disconnected with a reason).", AutomationSession::MaxNetworkClients), Schema::Object({ { "address", Schema::String("string: a UDP host to join, as host or host:port") } }), [](AutomationSession& session, const json& params) {
				RequireNetworkPlay(session);
				std::string error;
				if (params.contains("address"))
				{
					std::string address;
					uint16_t port = 0;
					if (const std::string problem = NetworkSettings::ParseAddress(RequireString(params, "address"), NetworkSettings::FromActiveProject().Port, address, port); !problem.empty())
						throw CommandError("address must be <host> or <host>:<port> with a port in [1, 65535]: " + problem);
					if (!session.JoinNetwork(address, port, error))
						throw CommandError(error);
					return json{ { "address", address }, { "port", port } };
				}
				const std::optional<size_t> client = session.AddNetworkClient(error);
				if (!client)
					throw CommandError(error);
				return json{ { "client", *client } };
			});

			Add(registry, "net.disconnect", "Ends an in-process client (client), or the playing scene's network session (without client). Peers are told (ClientQuit from a client, ServerShutdown from a host).", Schema::Object({ { "client", clientSchema } }), [](AutomationSession& session, const json& params) {
				RequireNetworkPlay(session);
				if (params.contains("client"))
				{
					std::string error;
					if (!session.RemoveNetworkClient(static_cast<size_t>(OptionalInteger(params, "client", 0, 1, MaxClientIndex)), error))
						throw CommandError(error);
					return json{ { "disconnected", true } };
				}
				if (!session.GetScene()->GetNetworkSystem())
					throw CommandError("the playing scene is in no network session");
				session.LeaveNetwork();
				return json{ { "disconnected", true } };
			});

			Add(registry, "net.status", "Describes the network session of the playing scene, or of an in-process client: role (Host, Client or null without a session), state (Connecting, Connected, Disconnected), peerId, disconnectReason, port (a host's), time and viewTime (the host clock, and the time a client shows other entities at), remoteEntities (a client's entities from the host), connections ({peer, address, roundTripTime, knownEntities, traffic}), and statistics (traffic, stale messages, spawns, adoptions, updates, despawns, rejected values and records, script messages sent, received and dropped, inputs applied, rejected and skipped, steps taken without inputs, corrections). Without client it also lists the in-process clients ({client, role, state, peerId}), which peer receives injected input (inputPeer) and, once the in-process network exists, the simulated link ({latency, jitter, loss, timeoutSeconds, lost}).", Schema::Object({ { "client", clientSchema } }), [](AutomationSession& session, const json& params) {
				if (params.contains("client"))
					return NetworkStatusJson(*RequireNetworkClient(session, params));
				json result = NetworkStatusJson(RequireScene(session));
				json clients = json::array();
				const std::vector<Ref<Scene>>& peers = session.GetNetworkClients();
				for (size_t i = 0; i < peers.size(); i++)
				{
					if (!peers[i])
						continue;
					json status = NetworkStatusJson(*peers[i]);
					clients.push_back({ { "client", i + 1 }, { "role", status["role"] }, { "state", status.value("state", json(nullptr)) }, { "peerId", status.value("peerId", json(nullptr)) } });
				}
				result["clients"] = std::move(clients);
				result["inputPeer"] = session.GetInputPeer();
				if (session.HasSimulatedNetwork())
					result["link"] = LinkJson(session.GetSimulatedNetwork());
				return result;
			});

			Add(registry, "net.link", "Sets the in-process network's link, both ways, for messages sent from now on: latency (one-way seconds), jitter (extra seconds up to this, per unreliable message, which reorders them), loss (chance in [0, 1] that a message is lost; a lost reliable one is sent again a round trip later) and timeoutSeconds (silence that ends a connection). Fields left out keep their values. Lasts until play stops. Returns the link.", Schema::Object({ { "latency", Schema::Nullable(Schema::Number("number of seconds in [0, 10]", 0.0, 10.0)) }, { "jitter", Schema::Nullable(Schema::Number("number of seconds in [0, 10]", 0.0, 10.0)) }, { "loss", Schema::Nullable(Schema::Number("number in [0, 1]", 0.0, 1.0)) }, { "timeoutSeconds", Schema::Nullable(Schema::Number("number of seconds in [1, 600]", 1.0, 600.0)) } }), [](AutomationSession& session, const json& params) {
				RequireNetworkPlay(session);
				SimulatedNetwork& network = session.GetSimulatedNetwork();
				SimulatedLinkSettings link = network.GetLinkSettings();
				link.Latency = static_cast<float>(OptionalNumber(params, "latency", link.Latency, 0.0, 10.0));
				link.Jitter = static_cast<float>(OptionalNumber(params, "jitter", link.Jitter, 0.0, 10.0));
				link.Loss = static_cast<float>(OptionalNumber(params, "loss", link.Loss, 0.0, 1.0));
				link.TimeoutSeconds = static_cast<float>(OptionalNumber(params, "timeoutSeconds", link.TimeoutSeconds, 1.0, 600.0));
				if (!SimulatedNetwork::IsValidLink(link))
					throw CommandError("the link settings are out of range");
				network.SetLinkSettings(link);
				return LinkJson(network);
			});

			Add(registry, "net.focus", "Chooses which peer injected input (input.key, input.mouse, input.gamepad) reaches while play.step runs: client 0 is the playing scene, N an in-process client. The others' games read no keys, buttons, mouse motion, scroll, text or gamepads (the pointer position stays visible). Resets to 0 when play stops or that client ends (net.disconnect, or its Game.Quit).", Schema::Object({ { "client", Schema::Integer("integer: 0 for the playing scene, or an in-process client's index", 0, MaxClientIndex) } }, { "client" }), [](AutomationSession& session, const json& params) {
				RequireNetworkPlay(session);
				const size_t client = static_cast<size_t>(OptionalInteger(params, "client", 0, 0, MaxClientIndex));
				if (!session.SetInputPeer(client))
					throw NoClientError(client);
				return json{ { "client", client } };
			});

			Add(registry, "net.command", fmt::format("Runs a command against an in-process client's scene instead of the playing scene, and returns its result: {}.", fmt::join(ClientCommands, ", ")), Schema::Object({ { "client", clientSchema }, { "command", Schema::String("string, the command to run", true) }, { "params", json{ { "type", "object" }, { "description", "object, the command's parameters" } } } }, { "client", "command" }), [&registry](AutomationSession& session, const json& params) {
				const Ref<Scene> client = RequireNetworkClient(session, params);
				const std::string command = RequireString(params, "command");
				if (std::ranges::none_of(ClientCommands, [&command](const char* allowed) { return command == allowed; }))
					throw CommandError(fmt::format("'{}' cannot run on a client (allowed: {})", command, fmt::join(ClientCommands, ", ")));
				const json commandParams = params.value("params", json::object());
				const AutomationSession::SceneOverride scope(session, client);
				return registry.Execute(session, command, commandParams);
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
		RegisterMaterials(registry);
		RegisterAnimationAuthoring(registry);
		RegisterPlay(registry);
		RegisterNetwork(registry);
		RegisterAssertions(registry);
		RegisterEvents(registry);
	}

}
