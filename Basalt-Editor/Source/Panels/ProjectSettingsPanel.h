#pragma once

#include "EditorContext.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>

#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	class Project;
	struct ProjectConfig;

	// Edits the active project's settings: name and start scene, game resolution, random seed, physics step,
	// graphics defaults, physics layers and their collision matrix, and runs project.validate. Every change goes
	// through project.set, so the window can do nothing the automation API cannot. Project settings are not scene
	// state: they are saved at once and are not part of undo history. The one scene edit here, a scene's own
	// physics step, goes through scene.settings and is undoable.
	class ProjectSettingsPanel
	{
	public:
		explicit ProjectSettingsPanel(EditorContext& context)
			: m_Context(context)
		{
		}

		// Draws the window while `open` is true; its close button clears it.
		void OnImGuiRender(bool& open);

	private:
		void DrawGeneral(const ProjectConfig& config);
		void DrawDisplay(const ProjectConfig& config);
		void DrawSimulation(const ProjectConfig& config);
		void DrawGraphics(const ProjectConfig& config);
		void DrawPhysicsLayers(const ProjectConfig& config);
		void DrawValidation();
		// Runs project.set, keeping the warnings it returns (undefined layers, an empty dynamic range, a scene that
		// keeps its own step).
		void Set(const nlohmann::json& params);

		// Draws a numeric field and reports its value once, when an edit of it ends. The project is re-read every
		// frame, so the value being edited is kept here: ImGui does not write a slider's (or a text field's) value
		// on the frame the edit ends, which is the frame it is committed on.
		template<typename T, size_t N, typename DrawField>
		bool EditCommitted(const char* label, std::array<T, N>& values, DrawField&& draw)
		{
			const ImGuiID id = ImGui::GetID(label);
			if (m_Edit.Id == id)
				for (size_t i = 0; i < N; i++)
					values[i] = static_cast<T>(m_Edit.Values[i]);
			draw(label, values.data());
			if (ImGui::IsItemActive())
			{
				m_Edit.Id = id;
				for (size_t i = 0; i < N; i++)
					m_Edit.Values[i] = static_cast<double>(values[i]);
				return false;
			}
			const bool committed = m_Edit.Id == id && ImGui::IsItemDeactivatedAfterEdit();
			if (m_Edit.Id == id)
				m_Edit.Id = 0;
			return committed;
		}

		struct FieldEdit
		{
			ImGuiID Id = 0;
			std::array<double, 2> Values{};
		};
		// A layer being renamed: which one (by its name when the edit began, so a change to the list meanwhile cannot
		// rename another layer) and the text typed so far.
		struct LayerRename
		{
			uint32_t Index = 0;
			std::string Original;
			std::string Buffer;
		};

		EditorContext& m_Context;
		const Project* m_Project = nullptr;
		std::string m_NameBuffer;
		bool m_EditingName = false;
		FieldEdit m_Edit;
		std::string m_NewLayerName;
		std::optional<LayerRename> m_Rename;
		std::vector<std::string> m_Warnings;
		// The last project.validate report, until the settings change.
		std::optional<nlohmann::json> m_Validation;
	};

}
