#include "Panels/ProjectSettingsPanel.h"

#include "EditorColors.h"

#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Automation/EditorCommandSchemas.h"
#include "Basalt/Core/GamepadCodes.h"
#include "Basalt/Core/InputActions.h"
#include "Basalt/Core/KeyCodes.h"
#include "Basalt/Core/MouseCodes.h"
#include "Basalt/Physics/FixedTimestep.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Renderer/RenderMath.h"
#include "Basalt/Scene/Scene.h"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <spdlog/fmt/fmt.h>

#include <array>
#include <cmath>
#include <random>
#include <span>
#include <string_view>

namespace Basalt {

	namespace {

		struct Resolution
		{
			const char* Label;
			uint32_t Width;
			uint32_t Height;
		};

		constexpr Resolution s_Resolutions[] = { { "1280 x 720 (16:9)", 1280, 720 }, { "1600 x 900 (16:9)", 1600, 900 }, { "1920 x 1080 (16:9)", 1920, 1080 }, { "2560 x 1440 (16:9)", 2560, 1440 }, { "1024 x 768 (4:3)", 1024, 768 }, { "1080 x 1920 (portrait)", 1080, 1920 } };
		constexpr int s_StepRates[] = { 30, 50, 60, 120 };

		// Pickers for the controls a binding can name, in the order of InputControlKeys.
		struct BindingSource
		{
			const char* Label;
			std::span<const std::string_view> (*Names)();
			bool IsGamepad;
			bool HasDeadZone;
		};

		constexpr std::array<BindingSource, InputControlKeys.size()> s_BindingSources = { {
			{ "Key", &GetKeyNames, false, false },
			{ "Mouse", &GetMouseButtonNames, false, false },
			{ "Gamepad Button", &GetGamepadButtonNames, true, false },
			{ "Gamepad Axis", &GetGamepadAxisNames, true, true },
		} };

		// A binding's gamepad index, or -1 for every gamepad.
		std::string GamepadLabel(int gamepad)
		{
			return gamepad >= 0 ? fmt::format("Gamepad {}", gamepad) : std::string("Any gamepad");
		}

		void HelpMarker(const char* text)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("(?)");
			if (ImGui::BeginItemTooltip())
			{
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
				ImGui::TextUnformatted(text);
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}
		}

	}

	void ProjectSettingsPanel::OnImGuiRender(bool& open)
	{
		if (!open)
			return;
		if (!ImGui::Begin(EditorCommandSchemas::ProjectSettingsTitle, &open))
		{
			ImGui::End();
			return;
		}
		const Ref<Project>& project = Project::GetActive();
		if (!project)
		{
			ImGui::TextDisabled("No project open.");
			ImGui::End();
			return;
		}
		// Another project: nothing shown so far applies to it.
		if (project != m_Project.lock())
		{
			m_Project = project;
			m_Validation.reset();
			m_Warnings.clear();
			m_Rename.reset();
			m_Edit = {};
			m_EditingName = false;
			m_NameBuffer.clear();
			m_NewLayerName.clear();
			m_NewActionName.clear();
		}
		// Project settings are not scene state, so they are saved at once and stay out of scene undo history.
		ImGui::TextDisabled("Saved to %s as you edit (not undoable).", Project::FileName);
		const bool playing = m_Context.Session.IsPlaying();
		if (playing)
			ImGui::TextColored(EditorColors::Warning, "Stop playing to edit: a session keeps the settings it started with.");
		for (const std::string& warning : m_Warnings)
			ImGui::TextColored(EditorColors::Warning, "%s", warning.c_str());
		// A copy: Set saves through project.set, which replaces the config being drawn.
		const ProjectConfig config = project->GetConfig();
		ImGui::BeginDisabled(playing);
		DrawGeneral(config);
		DrawDisplay(config);
		DrawSimulation(config);
		DrawGraphics(config);
		DrawPhysicsLayers(config);
		DrawInputActions(config);
		ImGui::EndDisabled();
		DrawValidation();
		ImGui::End();
	}

	bool ProjectSettingsPanel::Set(const nlohmann::json& params)
	{
		m_Warnings.clear();
		// The settings change (or the attempt shows them stale): validate again to see the effect.
		m_Validation.reset();
		const auto result = m_Context.Execute("project.set", params);
		if (!result)
			return false;
		if (const auto warnings = result->find("warnings"); warnings != result->end())
			for (const auto& warning : *warnings)
				m_Warnings.push_back(warning.get<std::string>());
		if (const auto layers = result->find("unknownPhysicsLayers"); layers != result->end())
			for (const auto& use : *layers)
				m_Warnings.push_back("'" + use["name"].get<std::string>() + "' uses the undefined layer '" + use["layer"].get<std::string>() + "' and plays on Default");
		return true;
	}

	void ProjectSettingsPanel::DrawGeneral(const ProjectConfig& config)
	{
		if (!ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_DefaultOpen))
			return;
		if (!m_EditingName)
			m_NameBuffer = config.Name;
		ImGui::InputText("Name", &m_NameBuffer);
		m_EditingName = ImGui::IsItemActive();
		if (ImGui::IsItemDeactivatedAfterEdit() && m_NameBuffer != config.Name)
			Set({ { "name", m_NameBuffer } });
		HelpMarker("Also names the exported executable and the game's save data directory.");

		if (ImGui::BeginCombo("Start Scene", config.StartScene.empty() ? "(none)" : config.StartScene.c_str()))
		{
			for (const std::string& scene : AssetManager::ListAssets({ ".bscene" }))
				if (ImGui::Selectable(scene.c_str(), scene == config.StartScene) && scene != config.StartScene)
					Set({ { "startScene", scene } });
			ImGui::EndCombo();
		}
	}

	void ProjectSettingsPanel::DrawDisplay(const ProjectConfig& config)
	{
		if (!ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen))
			return;
		const std::string current = std::to_string(config.WindowWidth) + " x " + std::to_string(config.WindowHeight);
		if (ImGui::BeginCombo("Game Resolution", current.c_str()))
		{
			for (const Resolution& resolution : s_Resolutions)
			{
				const bool selected = resolution.Width == config.WindowWidth && resolution.Height == config.WindowHeight;
				if (ImGui::Selectable(resolution.Label, selected) && !selected)
					Set({ { "windowWidth", resolution.Width }, { "windowHeight", resolution.Height } });
			}
			ImGui::EndCombo();
		}
		HelpMarker("The exported game's window size, and the size the editor's Game view renders at.");
		std::array<int, 2> size = { static_cast<int>(config.WindowWidth), static_cast<int>(config.WindowHeight) };
		if (EditCommitted("Custom", size, [](const char* id, int* values) { ImGui::InputInt2(id, values); }))
		{
			const auto clamp = [](int value) { return std::clamp(value, static_cast<int>(ProjectConfig::MinWindowSize), static_cast<int>(ProjectConfig::MaxWindowSize)); };
			const std::array<int, 2> clamped = { clamp(size[0]), clamp(size[1]) };
			if (clamped != size)
				m_Context.SetStatus(fmt::format("Window sizes are {} to {} pixels; using {} x {}", ProjectConfig::MinWindowSize, ProjectConfig::MaxWindowSize, clamped[0], clamped[1]), true);
			if (clamped[0] != static_cast<int>(config.WindowWidth) || clamped[1] != static_cast<int>(config.WindowHeight))
				Set({ { "windowWidth", clamped[0] }, { "windowHeight", clamped[1] } });
		}
		bool fullscreen = config.Fullscreen;
		if (ImGui::Checkbox("Fullscreen", &fullscreen))
			Set({ { "fullscreen", fullscreen } });
	}

	void ProjectSettingsPanel::DrawSimulation(const ProjectConfig& config)
	{
		if (!ImGui::CollapsingHeader("Simulation", ImGuiTreeNodeFlags_DefaultOpen))
			return;
		uint32_t seed = config.RandomSeed;
		if (EditCommitted("Random Seed", seed, [](const char* id, uint32_t* value) { ImGui::InputScalar(id, ImGuiDataType_U32, value); }) && seed != config.RandomSeed)
			Set({ { "randomSeed", seed } });
		ImGui::SameLine();
		if (ImGui::Button("Randomize"))
			Set({ { "randomSeed", std::random_device{}() } });
		HelpMarker("Seeds Lua's random numbers, particle emitters and runtime entity IDs, so a session replays identically.");

		// Physics step: presets by rate, or any value in seconds.
		const float step = config.FixedTimestep;
		const std::string label = fmt::format("{} Hz ({:.5f} s)", std::lround(1.0f / step), step);
		if (ImGui::BeginCombo("Physics Step", label.c_str()))
		{
			for (const int rate : s_StepRates)
			{
				const float seconds = 1.0f / static_cast<float>(rate);
				const std::string name = fmt::format("{} Hz", rate);
				const bool selected = std::abs(seconds - step) < 1e-6f;
				if (ImGui::Selectable(name.c_str(), selected) && !selected)
					Set({ { "fixedTimestep", seconds } });
			}
			ImGui::EndCombo();
		}
		float custom = step;
		if (EditCommitted("Step Seconds", custom, [](const char* id, float* value) { ImGui::InputFloat(id, value, 0.0f, 0.0f, "%.5f"); }) && custom != step)
		{
			if (IsValidFixedTimestep(custom))
				Set({ { "fixedTimestep", custom } });
			else
				m_Context.SetStatus(fmt::format("The physics step must be in (0, {}] seconds", MaxFixedTimestep), true);
		}

		// A scene may keep its own step (scene.settings); show it, since it hides the project value.
		Scene& scene = *m_Context.Session.GetScene();
		const float sceneStep = scene.GetPhysicsSettings().FixedTimestep;
		bool overrides = scene.GetPhysicsSettings().HasFixedTimestepOverride();
		if (ImGui::Checkbox("Open scene uses its own step", &overrides))
		{
			m_Context.Execute("scene.settings", { { "Physics", { { "FixedTimestep", overrides ? nlohmann::json(step) : nlohmann::json() } } } });
			m_Context.CommitHistory();
		}
		HelpMarker("A scene's own step (scene.settings Physics.FixedTimestep) overrides the project's. Unticking removes it, so the scene follows the project again.");
		if (overrides)
		{
			ImGui::Indent();
			ImGui::Text("This scene simulates at %.5f s (%d Hz).", sceneStep, static_cast<int>(std::lround(1.0f / sceneStep)));
			ImGui::Unindent();
		}
	}

	void ProjectSettingsPanel::DrawGraphics(const ProjectConfig& config)
	{
		if (!ImGui::CollapsingHeader("Graphics Defaults"))
			return;
		const GraphicsSettings& graphics = config.Graphics;
		bool vsync = graphics.VSync;
		if (ImGui::Checkbox("VSync", &vsync))
			Set({ { "graphics", { { "VSync", vsync } } } });
		float scale = graphics.RenderScale;
		if (EditCommitted("Render Scale", scale, [](const char* id, float* value) { ImGui::SliderFloat(id, value, MinRenderScale, MaxRenderScale, "%.2f"); }) && scale != graphics.RenderScale)
			Set({ { "graphics", { { "RenderScale", scale } } } });
		for (const auto& [key, quality] : { std::pair{ "ShadowQuality", graphics.ShadowQuality }, std::pair{ "SSAOQuality", graphics.SSAOQuality } })
		{
			int index = static_cast<int>(quality);
			if (ImGui::Combo(key, &index, GraphicsQualityNames, IM_ARRAYSIZE(GraphicsQualityNames)) && index != static_cast<int>(quality))
				Set({ { "graphics", { { key, GraphicsQualityNames[index] } } } });
		}
		bool dynamic = graphics.DynamicResolution.Enabled;
		if (ImGui::Checkbox("Dynamic Resolution", &dynamic))
			Set({ { "graphics", { { "DynamicResolution", dynamic } } } });
		ImGui::BeginDisabled(!dynamic);
		float target = graphics.DynamicResolution.TargetFrameRate;
		if (EditCommitted("Target Frame Rate", target, [](const char* id, float* value) { ImGui::SliderFloat(id, value, MinTargetFrameRate, MaxTargetFrameRate, "%.0f FPS"); }) && target != graphics.DynamicResolution.TargetFrameRate)
			Set({ { "graphics", { { "TargetFrameRate", target } } } });
		float minimum = graphics.DynamicResolution.MinScale;
		if (EditCommitted("Lowest Render Scale", minimum, [](const char* id, float* value) { ImGui::SliderFloat(id, value, MinRenderScale, MaxRenderScale, "%.2f"); }) && minimum != graphics.DynamicResolution.MinScale)
			Set({ { "graphics", { { "MinRenderScale", minimum } } } });
		ImGui::EndDisabled();
	}

	void ProjectSettingsPanel::DrawInputActions(const ProjectConfig& config)
	{
		if (!ImGui::CollapsingHeader("Input Actions", ImGuiTreeNodeFlags_DefaultOpen))
			return;
		ImGui::TextDisabled("Saved to %s. Scripts read them with Input.GetAction.", Project::InputFileName);
		HelpMarker("A Button action is as strong as its strongest binding; an Axis adds its bindings (a key with Scale -1 is the negative half). Scale multiplies a binding's value; a gamepad axis reads 0 inside its dead zone. Changes apply from the next play.");
		// Edited as the JSON project.set takes, so this panel can do nothing the automation API cannot.
		nlohmann::json actions = config.InputActions.ActionsToJson();
		bool changed = false;
		std::optional<size_t> removedAction;
		for (size_t i = 0; i < actions.size(); i++)
		{
			nlohmann::json& action = actions[i];
			ImGui::PushID(static_cast<int>(i));
			const std::string name = action["Name"].get<std::string>();
			const bool open = ImGui::TreeNodeEx("##Action", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap, "%s", name.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("Remove"))
				removedAction = i;
			if (open)
			{
				const InputActionType type = InputActionTypeFromString(action["Type"].get<std::string>()).value_or(InputActionType::Button);
				ImGui::SetNextItemWidth(120.0f);
				if (ImGui::BeginCombo("Type", std::string(InputActionTypeToString(type)).c_str()))
				{
					for (uint32_t t = 0; t < InputActionTypeCount; t++)
					{
						const auto option = static_cast<InputActionType>(t);
						const std::string label(InputActionTypeToString(option));
						if (ImGui::Selectable(label.c_str(), option == type) && option != type)
						{
							action["Type"] = label;
							changed = true;
						}
					}
					ImGui::EndCombo();
				}
				nlohmann::json& bindings = action["Bindings"];
				std::optional<size_t> removedBinding;
				for (size_t b = 0; b < bindings.size(); b++)
				{
					ImGui::PushID(static_cast<int>(b));
					changed |= DrawInputBinding(bindings[b]);
					ImGui::SameLine();
					if (ImGui::SmallButton("X"))
						removedBinding = b;
					ImGui::PopID();
				}
				if (removedBinding)
				{
					bindings.erase(*removedBinding);
					changed = true;
				}
				ImGui::BeginDisabled(bindings.size() >= InputActionMap::MaxBindings);
				if (ImGui::SmallButton("Add Binding"))
				{
					bindings.push_back({ { "Key", "Space" } });
					changed = true;
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (removedAction)
		{
			actions.erase(*removedAction);
			changed = true;
		}
		ImGui::SetNextItemWidth(180.0f);
		ImGui::InputTextWithHint("##NewAction", "New action name", &m_NewActionName);
		ImGui::SameLine();
		ImGui::BeginDisabled(m_NewActionName.empty() || config.InputActions.Find(m_NewActionName) || actions.size() >= InputActionMap::MaxActions);
		const bool add = ImGui::Button("Add Action");
		ImGui::EndDisabled();
		if (add)
			actions.push_back({ { "Name", m_NewActionName }, { "Type", "Button" }, { "Bindings", nlohmann::json::array() } });
		// The typed name stays when project.set rejects it (too long, control characters), so it can be fixed.
		if ((changed || add) && Set({ { "inputActions", { { "Actions", actions } } } }) && add)
			m_NewActionName.clear();
	}

	bool ProjectSettingsPanel::DrawInputBinding(nlohmann::json& binding)
	{
		const auto named = std::ranges::find_if(InputControlKeys, [&binding](std::string_view key) { return binding.contains(std::string(key)); });
		const size_t source = named != InputControlKeys.end() ? static_cast<size_t>(named - InputControlKeys.begin()) : 0;
		const BindingSource& current = s_BindingSources[source];
		const std::string key(InputControlKeys[source]);
		ImGui::SetNextItemWidth(130.0f);
		if (ImGui::BeginCombo("##Source", current.Label))
		{
			bool changed = false;
			for (size_t i = 0; i < s_BindingSources.size(); i++)
			{
				if (ImGui::Selectable(s_BindingSources[i].Label, i == source) && i != source)
				{
					// A new source starts at its first control; only Scale means the same for every source.
					nlohmann::json replaced = { { std::string(InputControlKeys[i]), std::string(s_BindingSources[i].Names().front()) } };
					if (binding.contains("Scale"))
						replaced["Scale"] = binding["Scale"];
					binding = std::move(replaced);
					changed = true;
				}
			}
			ImGui::EndCombo();
			if (changed)
				return true;
		}
		bool changed = false;
		ImGui::SameLine();
		const std::string control = binding[key].get<std::string>();
		ImGui::SetNextItemWidth(130.0f);
		if (ImGui::BeginCombo("##Control", control.c_str(), ImGuiComboFlags_HeightLarge))
		{
			for (const std::string_view name : current.Names())
			{
				const std::string label(name);
				if (ImGui::Selectable(label.c_str(), label == control) && label != control)
				{
					binding[key] = label;
					changed = true;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		float scale = binding.value("Scale", 1.0f);
		ImGui::SetNextItemWidth(70.0f);
		if (EditCommitted("Scale", scale, [](const char* label, float* value) { ImGui::DragFloat(label, value, 0.01f, -1.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp); }))
		{
			binding["Scale"] = scale;
			changed = true;
		}
		if (current.HasDeadZone)
		{
			ImGui::SameLine();
			float deadZone = binding.value("DeadZone", InputBinding::DefaultDeadZone);
			ImGui::SetNextItemWidth(70.0f);
			if (EditCommitted("Dead Zone", deadZone, [](const char* label, float* value) { ImGui::DragFloat(label, value, 0.01f, 0.0f, InputBinding::MaxDeadZone, "%.2f", ImGuiSliderFlags_AlwaysClamp); }))
			{
				binding["DeadZone"] = deadZone;
				changed = true;
			}
		}
		if (current.IsGamepad)
		{
			ImGui::SameLine();
			// -1 for every gamepad: plain ints, as GCC's -Wmaybe-uninitialized misfires on optional comparisons.
			const int selected = binding.contains("Gamepad") ? binding["Gamepad"].get<int>() : -1;
			ImGui::SetNextItemWidth(120.0f);
			if (ImGui::BeginCombo("##Gamepad", GamepadLabel(selected).c_str()))
			{
				for (int gamepad = -1; gamepad < static_cast<int>(Input::MaxGamepads); gamepad++)
				{
					if (ImGui::Selectable(GamepadLabel(gamepad).c_str(), gamepad == selected) && gamepad != selected)
					{
						if (gamepad >= 0)
							binding["Gamepad"] = gamepad;
						else
							binding.erase("Gamepad");
						changed = true;
					}
				}
				ImGui::EndCombo();
			}
		}
		return changed;
	}

	void ProjectSettingsPanel::DrawPhysicsLayers(const ProjectConfig& config)
	{
		if (!ImGui::CollapsingHeader("Physics Layers", ImGuiTreeNodeFlags_DefaultOpen))
			return;
		const PhysicsLayers& layers = config.Physics;
		const auto apply = [&](const PhysicsLayers& changed) { Set({ { "physicsLayers", changed.ToJson() } }); };
		std::string error;

		const auto& names = layers.GetNames();
		// A layer removed through automation while being renamed: nothing left to rename.
		if (m_Rename && m_Rename->Index >= layers.GetCount())
			m_Rename.reset();
		for (uint32_t i = 0; i < layers.GetCount(); i++)
		{
			ImGui::PushID(static_cast<int>(i));
			ImGui::Text("%2u", i);
			ImGui::SameLine();
			if (i == 0)
				ImGui::TextUnformatted(names[i].c_str());
			else
			{
				const bool renaming = m_Rename && m_Rename->Index == i;
				std::string buffer = renaming ? m_Rename->Buffer : names[i];
				ImGui::SetNextItemWidth(180.0f);
				if (ImGui::InputText("##Name", &buffer))
					m_Rename = LayerRename{ i, renaming ? m_Rename->Original : names[i], buffer };
				if (ImGui::IsItemDeactivatedAfterEdit() && m_Rename && m_Rename->Index == i)
				{
					// Only the layer the edit began on (the list may have changed through automation meanwhile).
					if (names[i] != m_Rename->Original)
						m_Context.SetStatus("The layers changed while renaming; the rename was dropped", true);
					else if (m_Rename->Buffer != names[i])
					{
						PhysicsLayers changed = layers;
						if (changed.Rename(i, m_Rename->Buffer, error))
							apply(changed);
						else
							m_Context.SetStatus(error, true);
					}
					m_Rename.reset();
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Remove"))
				{
					PhysicsLayers changed = layers;
					if (changed.Remove(i, error))
						apply(changed);
					else
						m_Context.SetStatus(error, true);
				}
			}
			ImGui::PopID();
		}
		ImGui::SetNextItemWidth(180.0f);
		ImGui::InputTextWithHint("##NewLayer", "New layer name", &m_NewLayerName);
		ImGui::SameLine();
		ImGui::BeginDisabled(m_NewLayerName.empty() || layers.GetCount() >= PhysicsLayers::MaxLayers);
		if (ImGui::Button("Add Layer"))
		{
			PhysicsLayers changed = layers;
			if (changed.Add(m_NewLayerName, error))
			{
				apply(changed);
				m_NewLayerName.clear();
			}
			else
				m_Context.SetStatus(error, true);
		}
		ImGui::EndDisabled();

		// Lower triangle of the symmetric matrix: row a, column b <= a, labelled by index (names are on the left).
		ImGui::Spacing();
		ImGui::TextUnformatted("Collision Matrix");
		HelpMarker("A ticked box makes bodies on the row's and the column's layers collide. Changes apply from the next play.");
		const int count = static_cast<int>(layers.GetCount());
		if (ImGui::BeginTable("CollisionMatrix", count + 1, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX, ImVec2(0.0f, ImGui::GetFrameHeightWithSpacing() * static_cast<float>(count + 2))))
		{
			ImGui::TableSetupColumn("Layer");
			for (int b = 0; b < count; b++)
				ImGui::TableSetupColumn(std::to_string(b).c_str());
			ImGui::TableHeadersRow();
			for (int a = 0; a < count; a++)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Text("%d %s", a, names[static_cast<size_t>(a)].c_str());
				for (int b = 0; b <= a; b++)
				{
					ImGui::TableNextColumn();
					ImGui::PushID(a * static_cast<int>(PhysicsLayers::MaxLayers) + b);
					bool collides = layers.ShouldCollide(static_cast<uint32_t>(a), static_cast<uint32_t>(b));
					if (ImGui::Checkbox("##Collides", &collides))
					{
						PhysicsLayers changed = layers;
						changed.SetCollides(static_cast<uint32_t>(a), static_cast<uint32_t>(b), collides);
						apply(changed);
					}
					if (ImGui::BeginItemTooltip())
					{
						ImGui::Text("%s x %s", names[static_cast<size_t>(a)].c_str(), names[static_cast<size_t>(b)].c_str());
						ImGui::EndTooltip();
					}
					ImGui::PopID();
				}
			}
			ImGui::EndTable();
		}
	}

	void ProjectSettingsPanel::DrawValidation()
	{
		if (!ImGui::CollapsingHeader("Validation", ImGuiTreeNodeFlags_DefaultOpen))
			return;
		if (ImGui::Button("Validate Project"))
			m_Validation = m_Context.Execute("project.validate");
		HelpMarker("Checks what would break the exported game: missing or wrongly cased asset paths, scenes and prefabs that do not load, scripts that do not compile (or, used by a Script component, do not return a class). Export runs the same check.");
		if (!m_Validation)
			return;
		const nlohmann::json& report = *m_Validation;
		if (report["valid"].get<bool>() && report["warnings"].get<size_t>() == 0)
			ImGui::TextColored(EditorColors::Success, "No problems found.");
		else
			ImGui::Text("%zu error(s), %zu warning(s)", report["errors"].get<size_t>(), report["warnings"].get<size_t>());
		for (const auto& issue : report["issues"])
		{
			const bool isError = issue["severity"] == "error";
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextColored(isError ? EditorColors::Error : EditorColors::Warning, "%s: %s", issue["file"].get<std::string>().c_str(), issue["message"].get<std::string>().c_str());
			ImGui::PopTextWrapPos();
		}
		if (report["omitted"].get<size_t>() > 0)
			ImGui::TextDisabled("... and %zu more (see project.validate)", report["omitted"].get<size_t>());
	}

}
