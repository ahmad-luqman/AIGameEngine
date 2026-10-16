#pragma once

#include <nlohmann/json.hpp>

namespace Basalt::EditorCommandSchemas {

	// Editor tool contracts live in the headless core so discovery and validation are testable without a GPU.
	nlohmann::json Pick();
	nlohmann::json VertexSnap();
	nlohmann::json Select();
	nlohmann::json Camera();
	nlohmann::json Transform();
	nlohmann::json ComponentSet();
	nlohmann::json BoxSelect();
	nlohmann::json Transaction();
	nlohmann::json Undo();
	nlohmann::json Redo();
	nlohmann::json WindowScreenshot();
	nlohmann::json Screenshot();
	nlohmann::json Panel();
	nlohmann::json Thumbnail();
	nlohmann::json MaterialPreview();

	// Editor windows editor.panel can show and focus (their ImGui window titles).
	inline constexpr const char* PanelNames[] = { "Viewport", "Game", "Hierarchy", "Inspector", "Scene", "Content Browser", "Console", "Project Settings" };
	// Of those, the windows that can be closed (the others are always open).
	inline constexpr const char* ClosablePanelNames[] = { "Project Settings" };

}
