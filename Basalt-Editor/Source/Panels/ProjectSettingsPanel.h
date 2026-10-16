#pragma once

#include "EditorContext.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	struct ProjectConfig;

	// Edits the active project's settings: name and start scene, game resolution, random seed, physics step,
	// graphics defaults, physics layers and their collision matrix, and runs project.validate. Every change goes
	// through project.set (or scene.settings for a scene's own physics step), so the window can do nothing
	// the automation API cannot. Project settings are saved immediately and are not part of undo history.
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
		// Runs project.set, keeping the warnings it returns (undefined layers, an empty dynamic range).
		void Set(const nlohmann::json& params);

		EditorContext& m_Context;
		std::string m_NameBuffer;
		bool m_EditingName = false;
		std::string m_NewLayerName;
		// The layer being renamed (index) and the text typed so far.
		std::optional<uint32_t> m_RenamingLayer;
		std::string m_RenameBuffer;
		std::vector<std::string> m_Warnings;
		std::optional<nlohmann::json> m_Validation;
	};

}
