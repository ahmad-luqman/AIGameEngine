#pragma once

#include "Basalt/Core/UUID.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <vector>

namespace Basalt {

	class Scene;
	class AutomationSession;
	class CommandRegistry;

	// Keeps unique selected roots so a transform or deletion visits descendants only once.
	std::vector<UUID> SelectionRoots(Scene& scene, const std::vector<UUID>& selection);
	// World-space delta between known affine gizmo pivots; retains the exact affine bottom row.
	glm::mat4 SelectionDelta(const glm::mat4& previous, const glm::mat4& next);
	// Mean world position and, in local mode, the active (last) entity's orientation.
	glm::mat4 SelectionPivot(Scene& scene, const std::vector<UUID>& selection, bool local);
	// Conservatively intersects world bounds with a viewport rectangle's frustum; excludes canvas UI.
	std::vector<UUID> SelectViewportBox(Scene& scene, const glm::mat4& viewProjection, const glm::vec2& size, const glm::vec2& start, const glm::vec2& end);
	// Applies already resolved UUIDs without per-entity command dispatch; preserves existing order.
	nlohmann::json ApplySelection(AutomationSession& session, const std::vector<UUID>& ids, const std::string& mode);
	// Prunes stale UUIDs and describes ordered membership and the active entity.
	nlohmann::json SelectionResult(AutomationSession& session);
	void RegisterEditorSelectionCommands(CommandRegistry& registry);

}
