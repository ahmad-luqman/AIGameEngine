#pragma once

#include "Basalt/Scene/Components.h"

#include <string>

namespace Basalt {

	// Strict finite TRS decomposition using basic arithmetic/sqrt only: libm hypot differs across platforms.
	bool DecomposeRagdollTransform(const glm::mat4& matrix, glm::vec3& position, glm::quat& rotation, glm::vec3& scale);

	// Deterministic shortest-arc recovery interpolation, using Jolt's portable trigonometry.
	glm::quat BlendRagdollRotation(const glm::quat& animated, const glm::quat& physical, float weight);

	// Shared by the registry and physics: C++ component writes meet the same rules as JSON/Lua edits.
	std::string ValidateRagdoll(const RagdollComponent& component);

}
