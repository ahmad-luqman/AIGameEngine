#pragma once

#include "EditorContext.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
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

		// Draws a numeric field and reports its value once, when an edit of it ends. The field is drawn from the
		// saved project every frame, which would overwrite what is being typed or dragged, so the value being edited
		// is kept here and drawn instead until the edit ends.
		template<typename T, size_t N, typename DrawField>
		bool EditCommitted(const char* label, std::array<T, N>& values, DrawField&& draw)
		{
			static_assert(std::is_arithmetic_v<T>, "the edit is kept as doubles");
			static_assert(N <= std::tuple_size_v<decltype(FieldEdit::Values)>, "FieldEdit holds at most two values");
			const ImGuiID id = ImGui::GetID(label);
			const bool tracked = m_Edit.Id == id;
			if (tracked)
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
			if (tracked)
				m_Edit.Id = 0;
			// Committed whenever ImGui says the edit ended, even if this field's values were not kept (another field
			// took the slot the same frame): the caller then sees the drawn values, at worst an unchanged setting.
			return ImGui::IsItemDeactivatedAfterEdit();
		}

		template<typename T, typename DrawField>
		bool EditCommitted(const char* label, T& value, DrawField&& draw)
		{
			std::array<T, 1> values = { value };
			const bool committed = EditCommitted(label, values, std::forward<DrawField>(draw));
			value = values[0];
			return committed;
		}

		struct FieldEdit
		{
			ImGuiID Id = 0;
			std::array<double, 2> Values{};
		};
		// A layer being renamed: its index, its name when the edit began (if the layer at that index has another name
		// by the time the edit ends, the list changed meanwhile and the rename is dropped) and the text typed so far.
		struct LayerRename
		{
			uint32_t Index = 0;
			std::string Original;
			std::string Buffer;
		};

		EditorContext& m_Context;
		// The project shown (weak, so a new project at the same address still counts as another).
		std::weak_ptr<Project> m_Project;
		std::string m_NameBuffer;
		bool m_EditingName = false;
		FieldEdit m_Edit;
		std::string m_NewLayerName;
		std::optional<LayerRename> m_Rename;
		std::vector<std::string> m_Warnings;
		// The last project.validate report, until this panel changes a setting or another project opens.
		std::optional<nlohmann::json> m_Validation;
	};

}
