#pragma once

#include "Basalt/Scene/Components.h"

#include <string>

namespace Basalt {

	// Strict finite TRS decomposition using basic arithmetic/sqrt only: libm hypot differs across platforms.
	bool DecomposeRagdollTransform(const glm::mat4& matrix, glm::vec3& position, glm::quat& rotation, glm::vec3& scale);

	// Shared by the registry and physics: C++ component writes meet the same rules as JSON/Lua edits.
	std::string ValidateRagdoll(const RagdollComponent& component);

	// Whether a live rig built from `built` must be rebuilt for `current`: only structural settings count. Control (Enabled,
	// RecoveryTime), motor and balance settings and each bone's MotorStrength apply live.
	bool RagdollNeedsRebuild(const RagdollComponent& built, const RagdollComponent& current);
	// How fast a KinematicRoot rig's root may be sent (m/s): past it the whole rig is carried along instead of swinging.
	inline constexpr float RagdollRootMaxSpeed = 30.0f;
	// Whether a root going by `offset` and from one rotation to the other in one fixed step was placed there rather than
	// walked: faster than RagdollRootMaxSpeed, or by more than a quarter turn at once. Following it would fling the limbs.
	bool IsRagdollTeleport(const glm::vec3& offset, const glm::quat& to, const glm::quat& from, float fixedStep);

}
