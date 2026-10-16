#include "Panels/ProjectSettingsPanel.h"

#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Physics/FixedTimestep.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Renderer/RenderMath.h"
#include "Basalt/Scene/Scene.h"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <spdlog/fmt/fmt.h>

#include <cmath>
#include <random>

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
		if (!ImGui::Begin("Project Settings", &open))
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
		// Saved immediately (they are the shipped game's settings), so they stay out of scene undo history.
		ImGui::TextDisabled("Saved to %s as you edit (not undoable).", Project::FileName);
		const bool playing = m_Context.Session.IsPlaying();
		if (playing)
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Stop playing to edit: a session keeps the settings it started with.");
		// A copy: Set saves through project.set, which replaces the config being drawn.
		const ProjectConfig config = project->GetConfig();
		ImGui::BeginDisabled(playing);
		DrawGeneral(config);
		DrawDisplay(config);
		DrawSimulation(config);
		DrawGraphics(config);
		DrawPhysicsLayers(config);
		ImGui::EndDisabled();
		DrawValidation();
		ImGui::End();
	}

	void ProjectSettingsPanel::Set(const nlohmann::json& params)
	{
		const auto result = m_Context.Execute("project.set", params);
		if (!result)
			return;
		m_Warnings.clear();
		if (const auto warnings = result->find("warnings"); warnings != result->end())
			for (const auto& warning : *warnings)
				m_Warnings.push_back(warning.get<std::string>());
		if (const auto layers = result->find("unknownPhysicsLayers"); layers != result->end())
			for (const auto& use : *layers)
				m_Warnings.push_back("'" + use["name"].get<std::string>() + "' uses the undefined layer '" + use["layer"].get<std::string>() + "' and plays on Default");
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
		int size[2] = { static_cast<int>(config.WindowWidth), static_cast<int>(config.WindowHeight) };
		ImGui::InputInt2("Custom", size);
		if (ImGui::IsItemDeactivatedAfterEdit())
			Set({ { "windowWidth", std::clamp(size[0], 64, 16384) }, { "windowHeight", std::clamp(size[1], 64, 16384) } });
		bool fullscreen = config.Fullscreen;
		if (ImGui::Checkbox("Fullscreen", &fullscreen))
			Set({ { "fullscreen", fullscreen } });
	}

	void ProjectSettingsPanel::DrawSimulation(const ProjectConfig& config)
	{
		if (!ImGui::CollapsingHeader("Simulation", ImGuiTreeNodeFlags_DefaultOpen))
			return;
		uint32_t seed = config.RandomSeed;
		ImGui::InputScalar("Random Seed", ImGuiDataType_U32, &seed);
		if (ImGui::IsItemDeactivatedAfterEdit() && seed != config.RandomSeed)
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
				const std::string name = std::to_string(rate) + " Hz";
				const bool selected = std::abs(seconds - step) < 1e-6f;
				if (ImGui::Selectable(name.c_str(), selected) && !selected)
					Set({ { "fixedTimestep", seconds } });
			}
			ImGui::EndCombo();
		}
		float custom = step;
		ImGui::InputFloat("Step Seconds", &custom, 0.0f, 0.0f, "%.5f");
		if (ImGui::IsItemDeactivatedAfterEdit() && custom != step)
		{
			if (IsValidFixedTimestep(custom))
				Set({ { "fixedTimestep", custom } });
			else
				m_Context.SetStatus(fmt::format("The physics step must be in (0, {}] seconds", MaxFixedTimestep), true);
		}

		// A scene may keep its own step (scene.settings); show it, since it hides the project value.
		Scene& scene = *m_Context.Session.GetScene();
		const float sceneStep = scene.GetPhysicsSettings().FixedTimestep;
		bool overrides = sceneStep > 0.0f;
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
		ImGui::SliderFloat("Render Scale", &scale, MinRenderScale, MaxRenderScale, "%.2f");
		if (ImGui::IsItemDeactivatedAfterEdit())
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
		ImGui::SliderFloat("Target Frame Rate", &target, MinTargetFrameRate, MaxTargetFrameRate, "%.0f FPS");
		if (ImGui::IsItemDeactivatedAfterEdit())
			Set({ { "graphics", { { "TargetFrameRate", target } } } });
		float minimum = graphics.DynamicResolution.MinScale;
		ImGui::SliderFloat("Lowest Render Scale", &minimum, MinRenderScale, MaxRenderScale, "%.2f");
		if (ImGui::IsItemDeactivatedAfterEdit())
			Set({ { "graphics", { { "MinRenderScale", minimum } } } });
		ImGui::EndDisabled();
	}

	void ProjectSettingsPanel::DrawPhysicsLayers(const ProjectConfig& config)
	{
		if (!ImGui::CollapsingHeader("Physics Layers", ImGuiTreeNodeFlags_DefaultOpen))
			return;
		const PhysicsLayers& layers = config.Physics;
		const auto apply = [&](const PhysicsLayers& changed) { Set({ { "physicsLayers", changed.ToJson() } }); };
		std::string error;

		const auto& names = layers.GetNames();
		for (uint32_t i = 0; i < layers.GetCount(); i++)
		{
			ImGui::PushID(static_cast<int>(i));
			ImGui::Text("%2u", i);
			ImGui::SameLine();
			if (i == 0)
				ImGui::TextUnformatted(names[i].c_str());
			else
			{
				std::string buffer = m_RenamingLayer == i ? m_RenameBuffer : names[i];
				ImGui::SetNextItemWidth(180.0f);
				if (ImGui::InputText("##Name", &buffer))
				{
					m_RenamingLayer = i;
					m_RenameBuffer = buffer;
				}
				if (ImGui::IsItemDeactivatedAfterEdit() && m_RenamingLayer == i)
				{
					PhysicsLayers changed = layers;
					if (m_RenameBuffer != names[i] && changed.Rename(i, m_RenameBuffer, error))
						apply(changed);
					else if (m_RenameBuffer != names[i])
						m_Context.SetStatus(error, true);
					m_RenamingLayer.reset();
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Remove"))
				{
					PhysicsLayers changed = layers;
					if (changed.Remove(i, error))
						apply(changed);
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
		for (const std::string& warning : m_Warnings)
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", warning.c_str());
	}

	void ProjectSettingsPanel::DrawValidation()
	{
		if (!ImGui::CollapsingHeader("Validation", ImGuiTreeNodeFlags_DefaultOpen))
			return;
		if (ImGui::Button("Validate Project"))
			m_Validation = m_Context.Execute("project.validate");
		HelpMarker("Checks what would break the exported game: missing or wrongly cased asset paths, scenes and prefabs that do not load, scripts that do not compile. Export runs the same check.");
		if (!m_Validation)
			return;
		const nlohmann::json& report = *m_Validation;
		if (report["valid"].get<bool>() && report["warnings"].get<size_t>() == 0)
			ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "No problems found.");
		else
			ImGui::Text("%zu error(s), %zu warning(s)", report["errors"].get<size_t>(), report["warnings"].get<size_t>());
		for (const auto& issue : report["issues"])
		{
			const bool isError = issue["severity"] == "error";
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextColored(isError ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f) : ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s: %s", issue["file"].get<std::string>().c_str(), issue["message"].get<std::string>().c_str());
			ImGui::PopTextWrapPos();
		}
		if (report["omitted"].get<size_t>() > 0)
			ImGui::TextDisabled("... and %zu more (see project.validate)", report["omitted"].get<size_t>());
	}

}
