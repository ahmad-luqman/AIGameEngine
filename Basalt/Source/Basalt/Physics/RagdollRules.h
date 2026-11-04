#pragma once

#include "Basalt/Scene/Components.h"

#include <string>

namespace Basalt {

	// Strict finite TRS decomposition using basic arithmetic/sqrt only: libm hypot differs across platforms.
	bool DecomposeRagdollTransform(const glm::mat4& matrix, glm::vec3& position, glm::quat& rotation, glm::vec3& scale);

	// Shared by the registry and physics: C++ component writes meet the same rules as JSON/Lua edits.
	std::string ValidateRagdoll(const RagdollComponent& component);

	// Whether a live rig built from `built` must be rebuilt for `current`: only structural settings count. Control
	// (Enabled, RecoveryTime) and motor settings apply live.
	bool RagdollNeedsRebuild(const RagdollComponent& built, const RagdollComponent& current);
	// Whether an entity that moved by `offset` and turned from `from` to `to` over `seconds` was placed there rather than
	// walked: faster than AnimationSystem::TeleportSpeed (over at least one fixed step, as physics moves bodies at most once
	// per step), or by more than a quarter turn at once. A root following either would fling what hangs from it.
	bool IsRagdollTeleport(const glm::vec3& offset, const glm::quat& to, const glm::quat& from, float seconds, float fixedStep);

}
