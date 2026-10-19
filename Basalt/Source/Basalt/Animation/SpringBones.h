#pragma once

#include "Basalt/Animation/AnimationPose.h"

#include <span>
#include <string>
#include <vector>

namespace Basalt {

	struct SpringChain;

	// Stateless pieces of spring bones (SpringBoneComponent): resolving a chain on a model and advancing its simulated
	// points. AnimationSystem keeps the points between frames and turns the joints toward them (SolveAimChainIK).

	// One simulated point: the child of a chain joint, and its velocity (world space).
	struct SpringPoint
	{
		glm::vec3 Position = glm::vec3(0);
		glm::vec3 Velocity = glm::vec3(0);
	};

	// A sphere (A == B) or capsule (the segment A..B) that points stay outside of, in world space.
	struct SpringCollisionShape
	{
		glm::vec3 A = glm::vec3(0);
		glm::vec3 B = glm::vec3(0);
		float Radius = 0;
	};

	// What SimulateSpringChain advances. Sizes: `Animated` holds the chain's joints and its tip (n + 1 positions), the
	// points are the children of the n joints (n positions).
	struct SpringSimulationRequest
	{
		// Where the animated pose puts the joints and the tip now, and at the previous simulation (world space). The
		// chain's first joint follows them exactly, interpolated across the substeps.
		std::span<const glm::vec3> Animated;
		std::span<const glm::vec3> PreviousAnimated;
		std::span<const SpringCollisionShape> Colliders;
		// Pull (1/s², 0..SpringChain::MaxStiffness) toward the animated shape, velocity decay (1/s, nonnegative), and the
		// points' radius against the colliders.
		float Stiffness = 0;
		float Damping = 0;
		float Radius = 0;
		// Gravity and wind (m/s²).
		glm::vec3 Acceleration = glm::vec3(0);
		float Seconds = 0;
	};
	// The longest substep is 1 / SpringStepRate seconds: a frame runs ceil(seconds * SpringStepRate) equal substeps.
	inline constexpr float SpringStepRate = 120.0f;
	// Substeps per frame at most. A longer frame (a hitch) simulates only its last MaxSpringSteps / SpringStepRate
	// seconds; the points first move along with the chain's first joint for the rest, so the hitch does not whip them.
	inline constexpr uint32_t MaxSpringSteps = 8;
	// How many times each substep alternates pushing a point out of the colliders and back to its bone length. A point
	// that can stay outside every collider at its length usually ends up there; overlapping colliders can leave it
	// slightly inside.
	inline constexpr uint32_t SpringConstraintIterations = 4;
	// Advances `points` (one per joint) by request.Seconds. Each point keeps its animated distance from the point
	// before it (from the first joint for the first point), is pulled toward where the animated chain shape puts it
	// relative to the simulated points before it, and is pushed out of the colliders.
	void SimulateSpringChain(const SpringSimulationRequest& request, std::span<SpringPoint> points);

	// Fills outJoints with the chain's joints followed by its tip (Joint, then each joint's first child, ChainLength
	// joints or, for ChainLength 0, down to the last joint that has a child, at most MaxChainLength) and returns empty,
	// or returns why it cannot (an empty or unknown joint, a joint without a child, fewer than ChainLength joints and a
	// tip in a line, a fixed-matrix joint other than the tip) and leaves outJoints empty.
	[[nodiscard]] std::string ResolveSpringChain(const MeshSource& source, const SpringChain& chain, std::vector<uint32_t>& outJoints);
	// The first node whose parent is `node`, or -1.
	int32_t FirstChild(const MeshSource& source, uint32_t node);
	// How many nodes have `node` as their parent.
	size_t ChildCount(const MeshSource& source, uint32_t node);

}
