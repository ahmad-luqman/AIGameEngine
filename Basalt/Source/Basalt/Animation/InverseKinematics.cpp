#include "Basalt/Animation/InverseKinematics.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace Basalt {

	namespace {
		float SafeAcos(float cosine)
		{
			return std::acos(std::clamp(cosine, -1.0f, 1.0f));
		}

		glm::vec3 Position(const std::vector<glm::mat4>& globals, uint32_t node)
		{
			return glm::vec3(globals[node][3]);
		}

		// The direction `from` turned toward `to` by at most maxAngle radians.
		glm::vec3 LimitDirection(const glm::vec3& from, const glm::vec3& to, float maxAngle)
		{
			const float angle = SafeAcos(glm::dot(from, to));
			if (angle <= maxAngle)
				return to;
			return glm::normalize(glm::slerp(glm::quat(1, 0, 0, 0), RotationBetween(from, to), maxAngle / angle) * from);
		}

		bool HasFixedMatrix(const MeshSource& source, std::initializer_list<uint32_t> nodes)
		{
			return std::ranges::any_of(nodes, [&](uint32_t node) { return source.Nodes[node].HasMatrix; });
		}

		// Blends the touched joints' rotations back toward `original` by weight, composes, and restores `original`
		// entirely when the result is not finite.
		bool Finalize(const MeshSource& source, AnimationLocalPose& pose, std::vector<glm::mat4>& globals, const AnimationLocalPose& original, const std::vector<uint32_t>& joints, float weight)
		{
			if (weight < 1.0f)
				for (uint32_t joint : joints)
					pose.Nodes[joint].Rotation = glm::normalize(glm::slerp(original.Nodes[joint].Rotation, pose.Nodes[joint].Rotation, weight));
			if (ComposeAnimationPose(source, pose, globals))
				return true;
			pose = original;
			ComposeAnimationPose(source, pose, globals);
			return false;
		}
	}

	glm::quat RotationBetween(const glm::vec3& from, const glm::vec3& to)
	{
		const float cosine = glm::dot(from, to);
		if (cosine < -0.999999f)
		{
			// Opposite directions: any axis perpendicular to `from` turns it half way round.
			glm::vec3 axis = glm::cross(glm::vec3(1, 0, 0), from);
			if (glm::dot(axis, axis) < 1e-8f)
				axis = glm::cross(glm::vec3(0, 1, 0), from);
			return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
		}
		const glm::vec3 axis = glm::cross(from, to);
		return glm::normalize(glm::quat(1.0f + cosine, axis.x, axis.y, axis.z));
	}

	glm::quat GlobalRotation(const glm::mat4& transform)
	{
		glm::mat3 basis(transform);
		for (int axis = 0; axis < 3; axis++)
		{
			const float length = glm::length(basis[axis]);
			if (length > 0.0f)
				basis[axis] /= length;
		}
		// A mirrored transform keeps its rotation with the reflection moved into X, as DecomposeTransform does.
		if (glm::determinant(basis) < 0.0f)
			basis[0] = -basis[0];
		return glm::normalize(glm::quat_cast(basis));
	}

	void RotateJointGlobal(const MeshSource& source, AnimationLocalPose& pose, const std::vector<glm::mat4>& globals, uint32_t node, const glm::quat& delta)
	{
		const int32_t parent = source.Nodes[node].Parent;
		const glm::quat parentRotation = parent >= 0 ? GlobalRotation(globals[parent]) : glm::quat(1, 0, 0, 0);
		// global' = delta * global, with global = parent * local.
		auto& rotation = pose.Nodes[node].Rotation;
		rotation = glm::normalize(glm::inverse(parentRotation) * delta * parentRotation * rotation);
	}

	bool SolveTwoBoneIK(const MeshSource& source, AnimationLocalPose& pose, std::vector<glm::mat4>& globals, uint32_t upper, uint32_t middle, uint32_t end, const glm::vec3& target, const glm::vec3& pole, const std::optional<glm::quat>& endRotation, float weight)
	{
		if (weight <= 0.0f || HasFixedMatrix(source, { upper, middle, end }))
			return false;
		const AnimationLocalPose original = pose;
		const glm::vec3 a = Position(globals, upper), b = Position(globals, middle), c = Position(globals, end);
		const float upperLength = glm::length(b - a), lowerLength = glm::length(c - b);
		const glm::vec3 toEnd = c - a;
		const float reach = upperLength + lowerLength;
		if (upperLength < 1e-6f || lowerLength < 1e-6f || glm::length(toEnd) < 1e-6f * reach || !std::isfinite(glm::length(target)))
			return false;
		// Just short of straight and of fully folded: both keep a bend plane for the next frame.
		const float epsilon = 1e-4f * reach;
		const float distance = std::clamp(glm::length(target - a), std::abs(upperLength - lowerLength) + epsilon, reach - epsilon);
		const glm::vec3 endDirection = glm::normalize(toEnd);

		// The animated bend plane; a straight chain bends toward the pole, or any perpendicular.
		glm::vec3 axis = glm::cross(toEnd, b - a);
		if (glm::length(axis) < 1e-6f * upperLength * glm::length(toEnd))
		{
			glm::vec3 hint = glm::length(pole) > 0.0f ? glm::normalize(pole) : glm::vec3(0, 0, 1);
			if (std::abs(glm::dot(hint, endDirection)) > 0.999f)
				hint = std::abs(endDirection.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
			axis = glm::cross(endDirection, hint);
		}
		axis = glm::normalize(axis);

		// Law of cosines (Holden, "Simple Two Joint IK"): bend so the end lands on the upper-to-end line at the
		// target's distance, then swing that line onto the target.
		const float upperAngle = SafeAcos(glm::dot(endDirection, glm::normalize(b - a)));
		const float middleAngle = SafeAcos(glm::dot(glm::normalize(a - b), glm::normalize(c - b)));
		const float upperTarget = SafeAcos((lowerLength * lowerLength - upperLength * upperLength - distance * distance) / (-2.0f * upperLength * distance));
		const float middleTarget = SafeAcos((distance * distance - upperLength * upperLength - lowerLength * lowerLength) / (-2.0f * upperLength * lowerLength));
		const glm::vec3 toTarget = target - a;
		const glm::quat swing = glm::length(toTarget) > 1e-6f * reach ? RotationBetween(endDirection, glm::normalize(toTarget)) : glm::quat(1, 0, 0, 0);
		// The middle joint's turn is expressed before the upper joint's, which then carries it along.
		RotateJointGlobal(source, pose, globals, middle, glm::angleAxis(middleTarget - middleAngle, axis));
		RotateJointGlobal(source, pose, globals, upper, swing * glm::angleAxis(upperTarget - upperAngle, axis));
		if (!ComposeAnimationPose(source, pose, globals))
		{
			pose = original;
			ComposeAnimationPose(source, pose, globals);
			return false;
		}

		if (glm::length(pole) > 0.0f && glm::length(toTarget) > 1e-6f * reach)
		{
			// Twist about the upper-to-target line until the middle joint faces the pole.
			const glm::vec3 line = glm::normalize(toTarget);
			const glm::vec3 knee = Position(globals, middle) - a;
			const glm::vec3 bend = knee - line * glm::dot(knee, line);
			const glm::vec3 side = pole - line * glm::dot(pole, line);
			if (glm::length(bend) > 1e-6f * reach && glm::length(side) > 1e-6f)
			{
				const float angle = std::atan2(glm::dot(line, glm::cross(bend, side)), glm::dot(bend, side));
				RotateJointGlobal(source, pose, globals, upper, glm::angleAxis(angle, line));
				ComposeAnimationPose(source, pose, globals);
			}
		}
		if (endRotation)
		{
			const int32_t parent = source.Nodes[end].Parent;
			const glm::quat parentRotation = parent >= 0 ? GlobalRotation(globals[parent]) : glm::quat(1, 0, 0, 0);
			pose.Nodes[end].Rotation = glm::normalize(glm::inverse(parentRotation) * *endRotation);
		}
		return Finalize(source, pose, globals, original, { upper, middle, end }, weight);
	}

	bool SolveLookAtIK(const MeshSource& source, AnimationLocalPose& pose, std::vector<glm::mat4>& globals, const std::vector<uint32_t>& joints, const glm::vec3& axis, const glm::vec3& target, float maxAngle, float weight)
	{
		if (weight <= 0.0f || joints.empty() || glm::length(axis) <= 0.0f || !std::isfinite(glm::length(target)))
			return false;
		if (std::ranges::any_of(joints, [&](uint32_t joint) { return source.Nodes[joint].HasMatrix; }))
			return false;
		const uint32_t aim = joints.front();
		const auto aimDirection = [&] { return glm::vec3(globals[aim] * glm::vec4(axis, 0.0f)); };
		const glm::vec3 animated = aimDirection();
		if (glm::length(animated) < 1e-8f)
			return false;
		const glm::vec3 animatedAim = glm::normalize(animated);
		const AnimationLocalPose original = pose;
		bool turned = false;
		// Outermost joint first; each takes an equal share of the turn still left.
		for (size_t remaining = joints.size(); remaining > 0; remaining--)
		{
			const glm::vec3 toTarget = target - Position(globals, aim);
			const glm::vec3 current = aimDirection();
			if (glm::length(toTarget) < 1e-6f || glm::length(current) < 1e-8f)
				break;
			turned = true;
			const glm::vec3 wanted = LimitDirection(animatedAim, glm::normalize(toTarget), maxAngle);
			const glm::quat turn = glm::slerp(glm::quat(1, 0, 0, 0), RotationBetween(glm::normalize(current), wanted), 1.0f / static_cast<float>(remaining));
			RotateJointGlobal(source, pose, globals, joints[remaining - 1], turn);
			if (!ComposeAnimationPose(source, pose, globals))
				break;
		}
		// A target at the aiming joint has no direction.
		if (!turned)
			return false;
		return Finalize(source, pose, globals, original, joints, weight);
	}

}
