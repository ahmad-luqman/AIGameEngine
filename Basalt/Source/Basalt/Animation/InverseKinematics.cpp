#include "Basalt/Animation/InverseKinematics.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Scene/Components.h"

#include <spdlog/fmt/fmt.h>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

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

		// Rotates `node` by the model-space rotation `delta` about its own origin (its descendants follow). Globals are
		// left stale; compose the pose afterwards.
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

		bool IsFree(const FabrikJointLimit& limit)
		{
			return limit.ConeAngle >= glm::pi<float>() && limit.MinTwist <= -glm::pi<float>() && limit.MaxTwist >= glm::pi<float>();
		}

		// `rotation` brought within `limit` of `rest`, measured about `axis` (the bone, unit, in the joint's own space).
		// Relative to rest it splits into a twist about the bone followed by a swing of the bone; each is limited alone, so a
		// joint at its cone keeps its twist.
		glm::quat LimitRotation(const glm::quat& rotation, const glm::quat& rest, const glm::vec3& axis, const FabrikJointLimit& limit)
		{
			glm::quat relative = glm::normalize(glm::inverse(rest) * rotation);
			// The shorter of the two rotations a quaternion and its negative stand for, so the angles below read -pi..pi.
			if (relative.w < 0.0f)
				relative = -relative;
			const float twist = 2.0f * std::atan2(glm::dot(glm::vec3(relative.x, relative.y, relative.z), axis), relative.w);
			glm::quat swing = glm::normalize(relative * glm::inverse(glm::angleAxis(twist, axis)));
			if (swing.w < 0.0f)
				swing = -swing;
			const float swingAngle = 2.0f * std::atan2(glm::length(glm::vec3(swing.x, swing.y, swing.z)), swing.w);
			if (swingAngle > limit.ConeAngle)
				swing = glm::slerp(glm::quat(1, 0, 0, 0), swing, limit.ConeAngle / swingAngle);
			return glm::normalize(rest * swing * glm::angleAxis(std::clamp(twist, limit.MinTwist, limit.MaxTwist), axis));
		}

		// The direction of `child` from its parent at rest, in the parent's own space; zero when it has no bone.
		glm::vec3 RestBoneAxis(const MeshSource& source, uint32_t node, uint32_t child)
		{
			const MeshNode& mesh = source.Nodes[child];
			const glm::vec3 offset = source.Nodes[node].Scale * (mesh.HasMatrix ? glm::vec3(mesh.Matrix[3]) : mesh.Translation);
			const float length = glm::length(offset);
			return length > 1e-9f ? offset / length : glm::vec3(0);
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
				return "a request value (target, pole or axis) or the solved pose is not finite";
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
		const uint32_t chainLength = std::clamp<uint32_t>(chain.ChainLength, 1, IKChain::MaxChainLength);
		// LookAt: the aiming joint and its ancestors.
		size_t length = chainLength;
		if (chain.Type == IKChainType::TwoBone)
			length = 3;
		// Fabrik's ChainLength counts bones, so the end joint comes on top.
		else if (chain.Type == IKChainType::Fabrik)
			length = chainLength + 1;
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
		else if (IKChainReaches(chain.Type))
			std::ranges::reverse(outJoints);
		return problem;
	}

	void ResolveFabrikLimits(const MeshSource& source, const IKChain& chain, std::span<const uint32_t> joints, std::vector<FabrikJointLimit>& outLimits, std::vector<std::string>& outProblems)
	{
		outLimits.clear();
		if (!HasIKJointLimits(chain))
			return;
		// In radians; nullopt for values ComponentRegistry would refuse.
		const auto convert = [](float cone, float minTwist, float maxTwist) -> std::optional<FabrikJointLimit> {
			const auto valid = [](float angle, float low, float high) { return std::isfinite(angle) && angle >= low && angle <= high; };
			if (!valid(cone, 0, IKChain::MaxConeAngle) || !valid(minTwist, -IKChain::MaxTwistAngle, IKChain::MaxTwistAngle) || !valid(maxTwist, minTwist, IKChain::MaxTwistAngle))
				return std::nullopt;
			return FabrikJointLimit{ .ConeAngle = glm::radians(cone), .MinTwist = glm::radians(minTwist), .MaxTwist = glm::radians(maxTwist) };
		};
		const std::optional<FabrikJointLimit> common = convert(chain.ConeAngle, chain.MinTwist, chain.MaxTwist);
		if (!common)
			outProblems.emplace_back("ConeAngle, MinTwist or MaxTwist is out of range; those joints stay free");
		const std::span<const uint32_t> turned = joints.first(joints.size() - 1);
		std::vector<FabrikJointLimit> limits(turned.size(), common.value_or(FabrikJointLimit{}));
		for (const IKJointLimit& limit : chain.JointLimits)
		{
			if (limit.Joint.empty())
				continue;
			const auto joint = std::ranges::find_if(turned, [&](uint32_t node) { return source.Nodes[node].Name == limit.Joint; });
			if (joint == turned.end())
			{
				outProblems.push_back(fmt::format("JointLimits: '{}' is not a joint the chain turns", limit.Joint));
				continue;
			}
			const std::optional<FabrikJointLimit> own = convert(limit.ConeAngle, limit.MinTwist, limit.MaxTwist);
			if (!own)
				outProblems.push_back(fmt::format("JointLimits: the limits of '{}' are out of range; it stays free", limit.Joint));
			limits[static_cast<size_t>(joint - turned.begin())] = own.value_or(FabrikJointLimit{});
		}
		if (!std::ranges::all_of(limits, IsFree))
			outLimits = std::move(limits);
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

	IKSolveResult SolveFabrikIK(const MeshSource& source, AnimationLocalPose& pose, std::vector<glm::mat4>& globals, const FabrikIKRequest& request)
	{
		const std::span<const uint32_t> joints = request.Joints;
		BS_CORE_ASSERT(joints.size() >= 2 && pose.Nodes.size() == source.Nodes.size() && globals.size() == source.Nodes.size(), "FABRIK needs two joints, and a pose and globals of the model");
		for (size_t i = 0; i < joints.size(); i++)
			BS_CORE_ASSERT(joints[i] < source.Nodes.size() && (i == 0 || source.Nodes[joints[i]].Parent == static_cast<int32_t>(joints[i - 1])), "FABRIK joints must form a parent-child chain of the model");
		const float weight = ClampWeight(request.Weight);
		if (weight <= 0.0f)
			return IKSolveResult::Unchanged;
		const std::span<const uint32_t> turned = joints.first(joints.size() - 1);
		if (HasFixedMatrix(source, turned) || (request.EndRotation && source.Nodes[joints.back()].HasMatrix))
			return IKSolveResult::FixedMatrix;
		if (!Math::IsFinite(request.Target) || (request.Pole && !Math::IsFinite(*request.Pole)) || (request.EndRotation && !Math::IsFinite(*request.EndRotation)))
			return IKSolveResult::NonFinite;

		const size_t bones = turned.size();
		BS_CORE_ASSERT(request.Limits.empty() || request.Limits.size() == bones, "one limit per turned joint");
		const bool limited = !request.Limits.empty();
		std::vector<glm::vec3> points(joints.size());
		std::vector<float> lengths(bones);
		// Each bone's animated direction: where a bone that collapses onto its neighbour during a pass keeps pointing.
		std::vector<glm::vec3> animated(bones);
		float reach = 0;
		points.front() = Position(globals, joints.front());
		for (size_t i = 0; i < bones; i++)
		{
			points[i + 1] = Position(globals, joints[i + 1]);
			reach += lengths[i] = glm::length(points[i + 1] - points[i]);
			animated[i] = lengths[i] > 1e-9f ? (points[i + 1] - points[i]) / lengths[i] : glm::vec3(0, 1, 0);
		}
		if (reach < 1e-6f)
			return IKSolveResult::Degenerate;
		const glm::vec3 root = points.front();
		const glm::vec3& target = request.Target;
		// Where a bone lies after a pass; a bone that collapsed onto its neighbour keeps the direction it had.
		const auto place = [](const glm::vec3& from, const glm::vec3& toward, const glm::vec3& fallback, float length) {
			const glm::vec3 direction = toward - from;
			const float distance = glm::length(direction);
			return from + (distance > 1e-9f ? direction / distance : fallback) * length;
		};
		const float tolerance = FabrikTolerance * reach;
		// A limited chain may not straighten: its passes find what it can do.
		if (!limited && glm::length(target - root) >= reach)
		{
			const glm::vec3 direction = glm::normalize(target - root);
			for (size_t i = 0; i < bones; i++)
				points[i + 1] = points[i] + direction * lengths[i];
		}
		else
		{
			// A straight chain stays straight under FABRIK's passes (it folds along its line), so one whose end is not already
			// at the target first bends its inner joints slightly toward the pole.
			const glm::vec3 line = points.back() - root;
			bool straight = glm::length(points.back() - target) > tolerance && glm::length(line) > 1e-6f * reach;
			for (size_t i = 1; straight && i < bones; i++)
				straight = glm::length(glm::cross(points[i] - root, line)) <= 1e-4f * reach * glm::length(line);
			if (straight)
			{
				const glm::vec3 direction = glm::normalize(line);
				glm::vec3 hint = request.Pole && glm::length(*request.Pole) > 0.0f ? glm::normalize(*request.Pole) : glm::vec3(0, 0, 1);
				if (std::abs(glm::dot(hint, direction)) > 0.999f)
					hint = std::abs(direction.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
				const glm::vec3 side = glm::normalize(hint - direction * glm::dot(hint, direction));
				for (size_t i = 1; i < bones; i++)
					points[i] += side * (0.01f * reach);
			}
			for (uint32_t iteration = 0; !limited && iteration < FabrikMaxIterations && glm::length(points.back() - target) > tolerance; iteration++)
			{
				points.back() = target;
				for (size_t i = bones; i-- > 0;)
					points[i] = place(points[i + 1], points[i], -animated[i], lengths[i]);
				points.front() = root;
				for (size_t i = 0; i < bones; i++)
					points[i + 1] = place(points[i], points[i + 1], animated[i], lengths[i]);
			}
		}
		if (!std::ranges::all_of(points, [](const glm::vec3& point) { return Math::IsFinite(point); }))
			return IKSolveResult::NonFinite;

		const SavedRotations saved(pose, joints);
		if (limited)
		{
			std::vector<glm::vec3> axes(bones);
			for (size_t i = 0; i < bones; i++)
				axes[i] = RestBoneAxis(source, joints[i], joints[i + 1]);
			const auto limitJoint = [&](size_t i) {
				const uint32_t joint = joints[i];
				if (axes[i] != glm::vec3(0) && !IsFree(request.Limits[i]))
					pose.Nodes[joint].Rotation = LimitRotation(pose.Nodes[joint].Rotation, source.Nodes[joint].Rotation, axes[i], request.Limits[i]);
			};
			// A forward pass in rotations: each joint turns toward its point, is limited, and moves the rest of the chain, so
			// points always describe a pose within the limits. Only the chain's own globals are kept up to date.
			const auto forward = [&] {
				for (size_t i = 0; i < bones; i++)
				{
					const uint32_t joint = joints[i], child = joints[i + 1];
					ComposeAnimationNode(source, pose, child, globals);
					const glm::vec3 origin = Position(globals, joint);
					const glm::vec3 bone = Position(globals, child) - origin;
					const glm::vec3 aim = points[i + 1] - origin;
					if (glm::length(bone) >= 1e-6f * reach && glm::length(aim) >= 1e-6f * reach)
						RotateJoint(source, pose, globals, joint, RotationBetween(glm::normalize(bone), glm::normalize(aim)));
					limitJoint(i);
					ComposeAnimationNode(source, pose, joint, globals);
					ComposeAnimationNode(source, pose, child, globals);
					points[i + 1] = Position(globals, child);
				}
			};
			// Keeps the rotations that brought the end closest to the target.
			std::vector<glm::quat> best(bones);
			for (size_t i = 0; i < bones; i++)
				best[i] = pose.Nodes[joints[i]].Rotation;
			float bestError = std::numeric_limits<float>::max();
			const auto keepBest = [&] {
				const float error = glm::length(Position(globals, joints.back()) - target);
				if (!(error < bestError))
					return false;
				bestError = error;
				for (size_t i = 0; i < bones; i++)
					best[i] = pose.Nodes[joints[i]].Rotation;
				return true;
			};
			const auto restoreBest = [&] {
				for (size_t i = 0; i < bones; i++)
					pose.Nodes[joints[i]].Rotation = best[i];
				return ComposeOrRestore(source, pose, globals, saved);
			};
			// The first pass only brings the animated pose inside the limits, which a pose at its target needs too.
			forward();
			keepBest();
			// The backward pass places each bone so the joint at its end can still point the bone already placed: the chain's
			// free joints then take up what a limited one cannot give, instead of every pass asking the same joint again. The
			// bone's frame is the last pass's turned onto its new direction, which the forward pass makes exact.
			const auto backward = [&](size_t i) {
				const glm::vec3 point = place(points[i + 1], points[i], -animated[i], lengths[i]);
				if (i + 1 == bones || axes[i + 1] == glm::vec3(0) || lengths[i] < 1e-6f * reach || lengths[i + 1] < 1e-6f * reach)
					return point;
				const uint32_t joint = joints[i], child = joints[i + 1];
				const glm::vec3 bone = glm::normalize(points[i + 1] - point);
				const glm::vec3 current = Position(globals, child) - Position(globals, joint);
				// Where the rest pose points the next bone, in this bone's frame.
				const glm::vec3 rest = glm::mat3(globals[joint]) * (source.Nodes[child].Rotation * axes[i + 1]);
				if (glm::length(current) < 1e-6f * reach || glm::length(rest) < 1e-6f)
					return point;
				const glm::vec3 reference = RotationBetween(glm::normalize(current), bone) * glm::normalize(rest);
				const glm::vec3 next = glm::normalize(points[i + 2] - points[i + 1]);
				const float angle = SafeAcos(glm::dot(reference, next));
				const float limit = request.Limits[i + 1].ConeAngle;
				if (angle <= limit)
					return point;
				const glm::quat correction = glm::slerp(glm::quat(1, 0, 0, 0), RotationBetween(reference, next), (angle - limit) / angle);
				return points[i + 1] - (correction * bone) * lengths[i];
			};
			for (uint32_t iteration = 0; iteration < FabrikMaxIterations && bestError > tolerance; iteration++)
			{
				points.back() = target;
				for (size_t i = bones; i-- > 0;)
					points[i] = backward(i);
				forward();
				keepBest();
			}
			if (!restoreBest())
				return IKSolveResult::NonFinite;
			// Clamped passes stall against tight limits: each undoes what the limits took from the one before. Turning one joint
			// at a time (cyclic coordinate descent) from the best of them stays inside the limits and keeps closing in.
			for (uint32_t sweep = 0; sweep < FabrikLimitedSweeps && bestError > tolerance; sweep++)
			{
				for (size_t i = bones; i-- > 0;)
				{
					const uint32_t joint = joints[i];
					const glm::vec3 origin = Position(globals, joint);
					const glm::vec3 toEnd = Position(globals, joints.back()) - origin;
					const glm::vec3 toTarget = target - origin;
					if (glm::length(toEnd) >= 1e-6f * reach && glm::length(toTarget) >= 1e-6f * reach)
						RotateJoint(source, pose, globals, joint, RotationBetween(glm::normalize(toEnd), glm::normalize(toTarget)));
					limitJoint(i);
					for (size_t below = i; below < joints.size(); below++)
						ComposeAnimationNode(source, pose, joints[below], globals);
				}
				// A sweep that came no closer has done what the limits allow.
				if (!keepBest())
					break;
			}
			if (!restoreBest())
				return IKSolveResult::NonFinite;
		}
		else
			for (size_t i = 0; i < bones; i++)
			{
				const glm::vec3 origin = Position(globals, joints[i]);
				const glm::vec3 bone = Position(globals, joints[i + 1]) - origin;
				const glm::vec3 aim = points[i + 1] - origin;
				if (glm::length(bone) < 1e-6f * reach || glm::length(aim) < 1e-6f * reach)
					continue;
				RotateJoint(source, pose, globals, joints[i], RotationBetween(glm::normalize(bone), glm::normalize(aim)));
				// The joints below aim from where this turn puts them.
				if (!ComposeOrRestore(source, pose, globals, saved))
					return IKSolveResult::NonFinite;
			}
		if (request.EndRotation)
			pose.Nodes[joints.back()].Rotation = glm::normalize(LocalRotationFor(source, globals, joints.back(), glm::normalize(*request.EndRotation)));
		return Finalize(source, pose, globals, saved, weight);
	}

	IKSolveResult SolveAimChainIK(const MeshSource& source, AnimationLocalPose& pose, std::vector<glm::mat4>& globals, const AimChainIKRequest& request)
	{
		const std::span<const uint32_t> joints = request.Joints;
		BS_CORE_ASSERT(joints.size() == request.Targets.size() + 1, "one target per turned joint");
		if (joints.size() != request.Targets.size() + 1)
			return IKSolveResult::Degenerate;
		const float weight = ClampWeight(request.Weight);
		if (weight <= 0 || request.Targets.empty())
			return IKSolveResult::Unchanged;
		const std::span<const uint32_t> turned = joints.first(request.Targets.size());
		if (HasFixedMatrix(source, turned))
			return IKSolveResult::FixedMatrix;
		if (!std::ranges::all_of(request.Targets, [](const glm::vec3& target) { return Math::IsFinite(target); }))
			return IKSolveResult::NonFinite;
		const SavedRotations saved(pose, turned);
		bool turnedAny = false;
		for (size_t i = 0; i < turned.size(); i++)
		{
			const glm::vec3 origin = Position(globals, joints[i]);
			const glm::vec3 bone = Position(globals, joints[i + 1]) - origin;
			const glm::vec3 aim = request.Targets[i] - origin;
			if (glm::length(bone) < 1e-6f || glm::length(aim) < 1e-6f)
				continue;
			RotateJoint(source, pose, globals, joints[i], RotationBetween(glm::normalize(bone), glm::normalize(aim)));
			// The joints below aim from where this turn puts them.
			if (!ComposeOrRestore(source, pose, globals, saved))
				return IKSolveResult::NonFinite;
			turnedAny = true;
		}
		if (!turnedAny)
			return IKSolveResult::Degenerate;
		if (weight >= 1.0f)
			return IKSolveResult::Solved;
		return Finalize(source, pose, globals, saved, weight);
	}

}
