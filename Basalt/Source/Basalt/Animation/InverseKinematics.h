#pragma once

#include "Basalt/Animation/AnimationPose.h"

#include <optional>

namespace Basalt {

	// Analytic IK solvers on a local pose and its composed model-space globals. They rewrite local rotations of
	// the chain's joints, blended from the incoming pose by `weight`, and leave `globals` composed from the result.
	// Every position and direction is in model space. False (pose and globals unchanged) when the chain is
	// degenerate (zero-length bones) or the result is not finite.

	// Turns `upper` and bends `middle` so `end` reaches `target`; an unreachable target leaves the chain fully
	// stretched toward it (just short of straight, so the bend direction survives). `pole` (nonzero) turns the
	// chain about the upper-to-target line so the middle joint points toward it; zero keeps the animated bend.
	// `endRotation` sets the end joint's model-space rotation.
	bool SolveTwoBoneIK(const MeshSource& source, AnimationLocalPose& pose, std::vector<glm::mat4>& globals, uint32_t upper, uint32_t middle, uint32_t end, const glm::vec3& target, const glm::vec3& pole, const std::optional<glm::quat>& endRotation, float weight);

	// Turns joints[0] (the aiming joint) and its listed ancestors (joints[1..], nearest first) so joints[0]'s local
	// `axis` points at `target`, the turn limited to `maxAngle` radians from the animated aim. Each joint takes an
	// equal share of what remains, outermost first.
	bool SolveLookAtIK(const MeshSource& source, AnimationLocalPose& pose, std::vector<glm::mat4>& globals, const std::vector<uint32_t>& joints, const glm::vec3& axis, const glm::vec3& target, float maxAngle, float weight);

	// The shortest rotation turning unit vector `from` onto unit vector `to`.
	glm::quat RotationBetween(const glm::vec3& from, const glm::vec3& to);
	// The rotation part of a model-space node transform (scale and mirroring removed).
	glm::quat GlobalRotation(const glm::mat4& transform);
	// Rotates `node` by the model-space rotation `delta` about its own origin (its descendants follow). Globals are
	// left stale; compose the pose afterwards.
	void RotateJointGlobal(const MeshSource& source, AnimationLocalPose& pose, const std::vector<glm::mat4>& globals, uint32_t node, const glm::quat& delta);

}
