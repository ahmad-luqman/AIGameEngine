#include "Basalt/Animation/InverseKinematics.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Scene/Components.h"

#include <spdlog/fmt/fmt.h>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
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

		float ClampWeight(float weight)
		{
			return std::isfinite(weight) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
		}

		bool HasFixedMatrix(const MeshSource& source, std::span<const uint32_t> nodes)
		{
			return std::ranges::any_of(nodes, [&](uint32_t node) { return source.Nodes[node].HasMatrix; });
		}

		// The direction `from` turned toward `to` by at most maxAngle radians.
		glm::vec3 LimitDirection(const glm::vec3& from, const glm::vec3& to, float maxAngle)
		{
			const float angle = SafeAcos(glm::dot(from, to));
			if (angle <= maxAngle)
				return to;
			return glm::normalize(glm::slerp(glm::quat(1, 0, 0, 0), RotationBetween(from, to), maxAngle / angle) * from);
		}

		// A rotation in the frame of `parent` (as GlobalRotation reads it) as the child's local rotation sees it.
		// GlobalRotation moves a mirroring into X, so in a mirrored parent's frame the rotation turns the other way:
		// conjugate it by that reflection.
		glm::quat Unmirrored(const glm::mat4& parent, const glm::quat& rotation)
		{
			return glm::determinant(glm::mat3(parent)) < 0.0f ? glm::quat(rotation.w, rotation.x, -rotation.y, -rotation.z) : rotation;
		}

		// The local rotation giving `node` the model-space rotation `rotation` (as GlobalRotation reads it).
		glm::quat LocalRotationFor(const MeshSource& source, const std::vector<glm::mat4>& globals, uint32_t node, const glm::quat& rotation)
		{
			const int32_t parent = source.Nodes[node].Parent;
			if (parent < 0)
				return rotation;
			return Unmirrored(globals[parent], glm::inverse(GlobalRotation(globals[parent])) * rotation);
		}

		// The chain joints' rotations before a solve, so a failed one restores just those.
		class SavedRotations
		{
		public:
			SavedRotations(const AnimationLocalPose& pose, std::span<const uint32_t> joints)
				: m_Joints(joints)
			{
				m_Rotations.reserve(joints.size());
				for (uint32_t joint : joints)
					m_Rotations.push_back(pose.Nodes[joint].Rotation);
			}
			void Restore(const MeshSource& source, AnimationLocalPose& pose, std::vector<glm::mat4>& globals) const
			{
				for (size_t i = 0; i < m_Joints.size(); i++)
					pose.Nodes[m_Joints[i]].Rotation = m_Rotations[i];
				// The saved pose composed before the solve, so this cannot fail.
				ComposeAnimationPose(source, pose, globals);
			}
			// Moves each joint's solved rotation back toward the saved one: weight 0 keeps the saved rotation.
			void Blend(AnimationLocalPose& pose, float weight) const
			{
				for (size_t i = 0; i < m_Joints.size(); i++)
				{
					auto& rotation = pose.Nodes[m_Joints[i]].Rotation;
					rotation = glm::normalize(glm::slerp(m_Rotations[i], rotation, weight));
				}
			}

		private:
			std::span<const uint32_t> m_Joints;
			std::vector<glm::quat> m_Rotations;
		};

		// Composes the solved pose, or restores the saved rotations when it is not finite.
		bool ComposeOrRestore(const MeshSource& source, AnimationLocalPose& pose, std::vector<glm::mat4>& globals, const SavedRotations& saved)
		{
			if (ComposeAnimationPose(source, pose, globals))
				return true;
			saved.Restore(source, pose, globals);
			return false;
		}

		IKSolveResult Finalize(const MeshSource& source, AnimationLocalPose& pose, std::vector<glm::mat4>& globals, const SavedRotations& saved, float weight)
		{
			if (weight < 1.0f)
				saved.Blend(pose, weight);
			return ComposeOrRestore(source, pose, globals, saved) ? IKSolveResult::Solved : IKSolveResult::NonFinite;
		}
	}

	void RotateJoint(const MeshSource& source, AnimationLocalPose& pose, const std::vector<glm::mat4>& globals, uint32_t node, const glm::quat& delta)
	{
		const int32_t parent = source.Nodes[node].Parent;
		glm::quat local = delta;
		if (parent >= 0)
		{
			const glm::quat parentRotation = GlobalRotation(globals[parent]);
			local = Unmirrored(globals[parent], glm::inverse(parentRotation) * delta * parentRotation);
		}
		auto& rotation = pose.Nodes[node].Rotation;
		rotation = glm::normalize(local * rotation);
	}

	const char* DescribeIKResult(IKSolveResult result)
	{
		switch (result)
		{
			case IKSolveResult::Solved:
			case IKSolveResult::Unchanged:
				return "";
			case IKSolveResult::FixedMatrix:
				return "a joint has a fixed matrix and cannot be turned";
			case IKSolveResult::Degenerate:
				return "the chain has zero-length bones, is fully folded, or its aiming axis vanishes (a zero-scaled joint)";
			case IKSolveResult::NonFinite:
				return "a target, pole, axis or the solved pose is not finite";
		}
		return "unknown result";
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
		if (glm::determinant(basis) < 0.0f)
			basis[0] = -basis[0];
		return glm::normalize(glm::quat_cast(basis));
	}

	std::string ResolveIKChain(const MeshSource& source, const IKChain& chain, std::vector<uint32_t>& outJoints)
	{
		outJoints.clear();
		if (chain.Joint.empty())
			return "Joint is empty";
		const int32_t end = FindNode(source, chain.Joint);
		if (end < 0)
			return fmt::format("joint '{}' not found in '{}'", chain.Joint, source.Key);
		const size_t length = chain.Type == IKChainType::TwoBone ? 3 : std::clamp<uint32_t>(chain.ChainLength, 1, IKChain::MaxChainLength);
		for (int32_t node = end; node >= 0 && outJoints.size() < length; node = source.Nodes[node].Parent)
			outJoints.push_back(static_cast<uint32_t>(node));
		std::string problem;
		if (outJoints.size() < length)
			problem = chain.Type == IKChainType::TwoBone ? fmt::format("joint '{}' needs a parent and a grandparent", chain.Joint) : fmt::format("joint '{}' has fewer than {} joints above it", chain.Joint, length - 1);
		// Names the joint, unlike HasFixedMatrix.
		else if (const auto fixed = std::ranges::find_if(outJoints, [&](uint32_t joint) { return source.Nodes[joint].HasMatrix; }); fixed != outJoints.end())
			problem = fmt::format("joint '{}' has a fixed matrix and cannot be turned", source.Nodes[*fixed].Name);
		if (!problem.empty())
			outJoints.clear();
		else if (chain.Type == IKChainType::TwoBone)
			std::ranges::reverse(outJoints);
		return problem;
	}

	IKSolveResult SolveTwoBoneIK(const MeshSource& source, AnimationLocalPose& pose, std::vector<glm::mat4>& globals, const TwoBoneIKRequest& request)
	{
		const uint32_t upper = request.Upper, middle = request.Middle, end = request.End;
		BS_CORE_ASSERT(pose.Nodes.size() == source.Nodes.size() && globals.size() == source.Nodes.size() && end < source.Nodes.size(), "two-bone IK needs a pose and globals of the model");
		BS_CORE_ASSERT(IsAncestor(source, upper, middle) && IsAncestor(source, middle, end), "two-bone IK joints must form a chain");
		const float weight = ClampWeight(request.Weight);
		if (weight <= 0.0f)
			return IKSolveResult::Unchanged;
		const std::array joints{ upper, middle, end };
		if (HasFixedMatrix(source, joints))
			return IKSolveResult::FixedMatrix;
		const glm::vec3& target = request.Target;
		if (!Math::IsFinite(target) || (request.Pole && !Math::IsFinite(*request.Pole)) || (request.EndRotation && !Math::IsFinite(*request.EndRotation)))
			return IKSolveResult::NonFinite;
		const SavedRotations saved(pose, joints);
		const glm::vec3 a = Position(globals, upper), b = Position(globals, middle), c = Position(globals, end);
		const float upperLength = glm::length(b - a), lowerLength = glm::length(c - b);
		const glm::vec3 toEnd = c - a;
		const float reach = upperLength + lowerLength;
		if (upperLength < 1e-6f || lowerLength < 1e-6f || glm::length(toEnd) < 1e-6f * reach)
			return IKSolveResult::Degenerate;
		const glm::vec3 toTarget = target - a;
		const bool hasTargetDirection = glm::length(toTarget) > 1e-6f * reach;
		const bool hasPole = request.Pole && glm::length(*request.Pole) > 0.0f;
		// Just short of straight and of fully folded: the middle joint stays off the upper-to-target line, so the pole
		// twist below keeps a bend direction and the angles stay away from acos(+-1). With very unequal bones the two
		// limits cross and the stretch limit wins, holding the chain just short of straight.
		const float epsilon = 1e-4f * reach;
		const float longest = reach - epsilon;
		const float shortest = std::min(std::abs(upperLength - lowerLength) + epsilon, longest);
		const float distance = std::clamp(glm::length(toTarget), shortest, longest);
		const glm::vec3 endDirection = glm::normalize(toEnd);

		// The animated bend plane. A straight chain bends toward the pole, else +Z, else X or Y when +Z lies along it.
		glm::vec3 axis = glm::cross(toEnd, b - a);
		if (glm::length(axis) < 1e-6f * upperLength * glm::length(toEnd))
		{
			glm::vec3 hint = hasPole ? glm::normalize(*request.Pole) : glm::vec3(0, 0, 1);
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
		const glm::quat swing = hasTargetDirection ? RotationBetween(endDirection, glm::normalize(toTarget)) : glm::quat(1, 0, 0, 0);
		// Both turns read the animated (not yet recomposed) globals, so the middle joint's bend is expressed in the
		// animated frame and the upper joint's turn then carries the bent lower bone along; the call order is free.
		RotateJoint(source, pose, globals, middle, glm::angleAxis(middleTarget - middleAngle, axis));
		RotateJoint(source, pose, globals, upper, swing * glm::angleAxis(upperTarget - upperAngle, axis));
		if (!ComposeOrRestore(source, pose, globals, saved))
			return IKSolveResult::NonFinite;

		if (hasPole && hasTargetDirection)
		{
			// Twist about the upper-to-target line until the middle joint faces the pole.
			const glm::vec3 line = glm::normalize(toTarget);
			const glm::vec3 knee = Position(globals, middle) - a;
			const glm::vec3 bend = knee - line * glm::dot(knee, line);
			const glm::vec3 side = *request.Pole - line * glm::dot(*request.Pole, line);
			if (glm::length(bend) > 1e-6f * reach && glm::length(side) > 1e-6f)
			{
				const float angle = std::atan2(glm::dot(line, glm::cross(bend, side)), glm::dot(bend, side));
				RotateJoint(source, pose, globals, upper, glm::angleAxis(angle, line));
				if (!ComposeOrRestore(source, pose, globals, saved))
					return IKSolveResult::NonFinite;
			}
		}
		if (request.EndRotation)
			pose.Nodes[end].Rotation = glm::normalize(LocalRotationFor(source, globals, end, glm::normalize(*request.EndRotation)));
		return Finalize(source, pose, globals, saved, weight);
	}

	IKSolveResult SolveLookAtIK(const MeshSource& source, AnimationLocalPose& pose, std::vector<glm::mat4>& globals, const LookAtIKRequest& request)
	{
		const std::span<const uint32_t> joints = request.Joints;
		BS_CORE_ASSERT(!joints.empty() && pose.Nodes.size() == source.Nodes.size() && globals.size() == source.Nodes.size(), "look-at IK needs a joint, and a pose and globals of the model");
		for (size_t i = 1; i < joints.size(); i++)
			BS_CORE_ASSERT(IsAncestor(source, joints[i], joints[i - 1]), "look-at IK joints must be ancestors of the aiming joint");
		const float weight = ClampWeight(request.Weight);
		if (weight <= 0.0f)
			return IKSolveResult::Unchanged;
		if (HasFixedMatrix(source, joints))
			return IKSolveResult::FixedMatrix;
		if (!Math::IsFinite(request.Target) || !Math::IsFinite(request.Axis) || !std::isfinite(request.MaxAngleRadians))
			return IKSolveResult::NonFinite;
		const uint32_t aim = joints.front();
		const auto aimDirection = [&] { return glm::vec3(globals[aim] * glm::vec4(request.Axis, 0.0f)); };
		const glm::vec3 animated = aimDirection();
		if (glm::length(animated) < 1e-8f)
			return IKSolveResult::Degenerate;
		const glm::vec3 animatedAim = glm::normalize(animated);
		const float maxAngle = std::clamp(request.MaxAngleRadians, 0.0f, glm::pi<float>());
		// Relative to the aiming joint's scale (the axis's length in model space).
		const float onJoint = 1e-6f * glm::length(animated);
		const SavedRotations saved(pose, joints);
		for (size_t remaining = joints.size(); remaining > 0; remaining--)
		{
			const glm::vec3 toTarget = request.Target - Position(globals, aim);
			if (glm::length(toTarget) < onJoint)
			{
				// A target on the aiming joint has no direction: keep the animation.
				saved.Restore(source, pose, globals);
				return IKSolveResult::Unchanged;
			}
			const glm::vec3 wanted = LimitDirection(animatedAim, glm::normalize(toTarget), maxAngle);
			const glm::quat turn = glm::slerp(glm::quat(1, 0, 0, 0), RotationBetween(glm::normalize(aimDirection()), wanted), 1.0f / static_cast<float>(remaining));
			RotateJoint(source, pose, globals, joints[remaining - 1], turn);
			if (!ComposeOrRestore(source, pose, globals, saved))
				return IKSolveResult::NonFinite;
		}
		return Finalize(source, pose, globals, saved, weight);
	}

}
