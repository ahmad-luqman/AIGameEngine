#pragma once

#include <nlohmann/json.hpp>

namespace Basalt::EditorCommandSchemas {

	// Editor tool contracts live in the headless core so discovery and validation are testable without a GPU.
	nlohmann::json Pick();
	nlohmann::json VertexSnap();
	nlohmann::json Select();
	nlohmann::json Camera();
	nlohmann::json Undo();
	nlohmann::json Redo();
	nlohmann::json WindowScreenshot();
	nlohmann::json Screenshot();

}
