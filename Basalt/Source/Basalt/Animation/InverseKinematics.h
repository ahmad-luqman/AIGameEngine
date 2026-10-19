#pragma once

#include "Basalt/Animation/AnimationPose.h"

#include <glm/gtc/constants.hpp>

#include <optional>
#include <span>
#include <string>

namespace Basalt {

	struct IKChain;

	// Analytic IK solvers on a local pose and its model-space globals, which must be composed from that pose on
	// entry. They rewrite local rotations of the chain's joints, blended from the incoming pose by the request's
	// weight (clamped to 0..1), and leave `globals` composed from the result. Positions and directions are in model
	// space. Parent rotations are treated as rigid: a non-uniformly scaled parent makes the result approximate.

	enum class IKSolveResult : uint8_t
	{
		Solved,
		// Nothing to do: zero weight, or a target on the aiming joint (no direction to aim in).
		Unchanged,
		// A chain joint uses a fixed glTF matrix, which the pose cannot turn.
		FixedMatrix,
		// Zero-length bones, a fully folded chain, or an aiming axis that vanishes (a zero-scaled aiming joint).
		Degenerate,
		// A non-finite request value or result; pose and globals are left as they were.
		NonFinite
	};
	// Why a solve failed, for diagnostics; empty for Solved and Unchanged.
	const char* DescribeIKResult(IKSolveResult result);

	// What SolveTwoBoneIK solves: three joints and where the last one should go.
	struct TwoBoneIKRequest
	{
		// Each a descendant of the previous one (usually its child).
		uint32_t Upper = 0, Middle = 0, End = 0;
		glm::vec3 Target = glm::vec3(0);
		// The direction the middle joint bends toward; nullopt or zero keeps the animated bend (a straight chain then
		// bends toward +Z).
		std::optional<glm::vec3> Pole;
		// The end joint's model-space rotation; nullopt keeps its local rotation (it turns with the lower bone). Exact
		// unless the end joint itself is mirrored on Y or Z.
		std::optional<glm::quat> EndRotation;
		float Weight = 1.0f;
	};
	// Turns `Upper` and bends `Middle` so `End` reaches the target. Targets beyond the chain's reach (or closer than
	// the difference of its bones) leave it stretched (or folded) toward them, just short of straight (or of fully
	// folded).
	[[nodiscard]] IKSolveResult SolveTwoBoneIK(const MeshSource& source, AnimationLocalPose& pose, std::vector<glm::mat4>& globals, const TwoBoneIKRequest& request);

	// What SolveLookAtIK solves: the joints that turn and where the aiming joint's axis should point.
	struct LookAtIKRequest
	{
		// The aiming joint, then the ancestors that share the turn (nearest first).
		std::span<const uint32_t> Joints;
		// The aiming direction in the aiming joint's local space.
		glm::vec3 Axis = glm::vec3(0, 0, 1);
		glm::vec3 Target = glm::vec3(0);
		// The largest turn away from the animated aim.
		float MaxAngleRadians = glm::pi<float>();
		float Weight = 1.0f;
	};
	// Turns the joints so the aiming joint's axis points at the target. Each joint takes an equal share of the turn
	// still left, outermost first, so the aim is exact within MaxAngleRadians.
	[[nodiscard]] IKSolveResult SolveLookAtIK(const MeshSource& source, AnimationLocalPose& pose, std::vector<glm::mat4>& globals, const LookAtIKRequest& request);

	// Fills outJoints with the joints `chain` drives on `source` (TwoBone: upper, middle, end; LookAt: the aiming joint,
	// then its ancestors; ChainLength clamped to 1..MaxChainLength) and returns empty, or returns why it cannot (an empty
	// or unknown joint, too few ancestors, a fixed-matrix joint) and leaves outJoints empty.
	[[nodiscard]] std::string ResolveIKChain(const MeshSource& source, const IKChain& chain, std::vector<uint32_t>& outJoints);

	// The shortest rotation turning unit vector `from` onto unit vector `to`.
	glm::quat RotationBetween(const glm::vec3& from, const glm::vec3& to);
	// The rotation part of a model-space node transform: scale removed, and a mirroring moved into X (as
	// Math::DecomposeTransform does), so a mirrored transform still yields a proper rotation.
	glm::quat GlobalRotation(const glm::mat4& transform);
	// Rotates `node` by the model-space rotation `delta` about its own origin (its descendants follow), under mirrored
	// parents too. Globals are left stale; compose the pose afterwards.
	void RotateJoint(const MeshSource& source, AnimationLocalPose& pose, const std::vector<glm::mat4>& globals, uint32_t node, const glm::quat& delta);

}
