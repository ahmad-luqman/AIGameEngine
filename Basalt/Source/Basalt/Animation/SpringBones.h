#pragma once

#include "Basalt/Animation/InverseKinematics.h"

#include <span>
#include <string>
#include <vector>

namespace Basalt {

	struct SpringChain;

	// Stateless pieces of spring bones (SpringBoneComponent): resolving a chain on a model, advancing its simulated
	// points, and turning its joints toward them. AnimationSystem keeps the points between frames.

	// One simulated point: the child of a chain joint (world space), and where it was one step earlier (Verlet).
	struct SpringPoint
	{
		glm::vec3 Position = glm::vec3(0);
		glm::vec3 Previous = glm::vec3(0);
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
		// Pull (1/s²) toward the animated shape, velocity decay (1/s), and the points' radius against the colliders.
		float Stiffness = 0;
		float Damping = 0;
		float Radius = 0;
		// Gravity and wind (m/s²).
		glm::vec3 Acceleration = glm::vec3(0);
		float Seconds = 0;
	};
	// Simulation steps per second; a frame runs ceil(seconds * rate) substeps of equal length.
	inline constexpr float SpringStepRate = 120.0f;
	// Substeps per frame at most: a longer frame (a hitch) simulates only MaxSpringSteps / SpringStepRate seconds, so
	// stiff springs stay stable.
	inline constexpr uint32_t MaxSpringSteps = 8;
	// Advances `points` (one per joint) by request.Seconds. Each point keeps its animated distance from the point
	// before it (from the first joint for the first point), is pulled toward where the animated chain shape puts it
	// relative to the simulated points before it, and is pushed out of the colliders.
	void SimulateSpringChain(const SpringSimulationRequest& request, std::span<SpringPoint> points);

	// Fills outJoints with the chain's joints followed by its tip (Joint, then each joint's first child, ChainLength
	// joints or, for ChainLength 0, down to the last joint that has a child) and returns empty, or returns why it cannot
	// (an empty or unknown joint, a joint without a child, a fixed-matrix joint) and leaves outJoints empty.
	[[nodiscard]] std::string ResolveSpringChain(const MeshSource& source, const SpringChain& chain, std::vector<uint32_t>& outJoints);
	// The first node whose parent is `node`, or -1.
	int32_t FirstChild(const MeshSource& source, uint32_t node);

	// Turns each of the chain's joints (all of `joints` but the tip, parent first) so its child points at the matching
	// model-space target, blended from the incoming pose by `weight`; `globals` must be composed from `pose` and stays
	// composed. Joints whose bone or target direction vanishes keep their rotation.
	[[nodiscard]] IKSolveResult ApplySpringChain(const MeshSource& source, AnimationLocalPose& pose, std::vector<glm::mat4>& globals, std::span<const uint32_t> joints, std::span<const glm::vec3> targets, float weight);

}
