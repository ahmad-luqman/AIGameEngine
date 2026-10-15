#pragma once

#include "Basalt/Core/UUID.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <vector>

namespace Basalt {

	class Scene;
	class AutomationSession;
	class CommandRegistry;

	// Shared hierarchy-aware operations used by the editor GUI and its automation commands.
	std::vector<UUID> SelectionRoots(Scene& scene, const std::vector<UUID>& selection);
	glm::mat4 SelectionPivot(Scene& scene, const std::vector<UUID>& selection, bool local);
	std::vector<UUID> SelectViewportBox(Scene& scene, const glm::mat4& viewProjection, const glm::vec2& size, const glm::vec2& start, const glm::vec2& end);
	nlohmann::json SelectionResult(AutomationSession& session);
	void RegisterEditorSelectionCommands(CommandRegistry& registry);

}
