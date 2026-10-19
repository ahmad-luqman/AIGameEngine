#include "Basalt/Animation/SpringBones.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Scene/Components.h"

#include <spdlog/fmt/fmt.h>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace Basalt {

	namespace {
		constexpr float Epsilon = 1e-6f;

		// `point` moved to `length` from `head` along its direction; along `fallback`'s direction when it sits on the
		// head.
		glm::vec3 KeepLength(const glm::vec3& head, const glm::vec3& point, float length, const glm::vec3& fallback)
		{
			glm::vec3 offset = point - head;
			if (glm::length(offset) < Epsilon)
				offset = fallback - head;
			if (glm::length(offset) < Epsilon)
				return head;
			return head + glm::normalize(offset) * length;
		}

		// `point` pushed out of the shape grown by `radius`; `away` breaks the tie when the point is on its axis.
		glm::vec3 PushOut(const SpringCollisionShape& shape, const glm::vec3& point, float radius, const glm::vec3& away)
		{
			const glm::vec3 axis = shape.B - shape.A;
			const float lengthSquared = glm::dot(axis, axis);
			const float t = lengthSquared > 0 ? std::clamp(glm::dot(point - shape.A, axis) / lengthSquared, 0.0f, 1.0f) : 0.0f;
			const glm::vec3 closest = shape.A + axis * t;
			const float reach = shape.Radius + radius;
			glm::vec3 offset = point - closest;
			const float distance = glm::length(offset);
			if (distance >= reach)
				return point;
			if (distance < Epsilon)
			{
				offset = away - glm::dot(away, axis) * axis / std::max(lengthSquared, Epsilon);
				// Any direction across the axis.
				if (glm::length(offset) < Epsilon)
					offset = lengthSquared > 0 && std::abs(axis.y) > 0.9f * std::sqrt(lengthSquared) ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
			}
			return closest + glm::normalize(offset) * reach;
		}
	}

	void SimulateSpringChain(const SpringSimulationRequest& request, std::span<SpringPoint> points)
	{
		const size_t count = points.size();
		BS_CORE_ASSERT(request.Animated.size() == count + 1 && request.PreviousAnimated.size() == count + 1, "one animated position per joint plus the tip");
		const float seconds = std::min(request.Seconds, static_cast<float>(MaxSpringSteps) / SpringStepRate);
		if (count == 0 || !(seconds > 0))
			return;
		const uint32_t steps = std::clamp(static_cast<uint32_t>(std::ceil(seconds * SpringStepRate)), 1u, MaxSpringSteps);
		const float step = seconds / static_cast<float>(steps);
		const float decay = std::exp(-request.Damping * step);
		std::vector<glm::vec3> animated(count + 1);
		for (uint32_t substep = 1; substep <= steps; substep++)
		{
			// The animated pose moves evenly through the frame, so a fast-moving character does not jerk its points
			// once per frame.
			const float progress = static_cast<float>(substep) / static_cast<float>(steps);
			for (size_t i = 0; i <= count; i++)
				animated[i] = glm::mix(request.PreviousAnimated[i], request.Animated[i], progress);
			glm::vec3 head = animated[0];
			// How far the simulated chain so far turned away from the animated one: the animated shape below follows it.
			glm::quat turn(1, 0, 0, 0);
			for (size_t i = 0; i < count; i++)
			{
				SpringPoint& point = points[i];
				const glm::vec3 bone = animated[i + 1] - animated[i];
				const float length = glm::length(bone);
				const glm::vec3 target = head + turn * bone;
				const glm::vec3 velocity = (point.Position - point.Previous) * decay;
				const glm::vec3 acceleration = request.Stiffness * (target - point.Position) + request.Acceleration;
				glm::vec3 next = KeepLength(head, point.Position + velocity + acceleration * step * step, length, target);
				for (const SpringCollisionShape& shape : request.Colliders)
					next = PushOut(shape, next, request.Radius, next - head);
				next = KeepLength(head, next, length, target);
				point.Previous = point.Position;
				point.Position = next;
				if (length > Epsilon && glm::length(next - head) > Epsilon)
					turn = RotationBetween(bone / length, glm::normalize(next - head));
				head = next;
			}
		}
	}

	int32_t FirstChild(const MeshSource& source, uint32_t node)
	{
		const auto found = std::ranges::find(source.Nodes, static_cast<int32_t>(node), &MeshNode::Parent);
		return found == source.Nodes.end() ? -1 : static_cast<int32_t>(found - source.Nodes.begin());
	}

	std::string ResolveSpringChain(const MeshSource& source, const SpringChain& chain, std::vector<uint32_t>& outJoints)
	{
		outJoints.clear();
		if (chain.Joint.empty())
			return "Joint is empty";
		const int32_t first = FindNode(source, chain.Joint);
		if (first < 0)
			return fmt::format("joint '{}' not found in '{}'", chain.Joint, source.Key);
		const size_t length = chain.ChainLength == 0 ? SpringChain::MaxChainLength : std::min(chain.ChainLength, SpringChain::MaxChainLength);
		outJoints.push_back(static_cast<uint32_t>(first));
		// The joints, then the tip.
		while (outJoints.size() <= length)
		{
			const int32_t child = FirstChild(source, outJoints.back());
			if (child < 0)
				break;
			outJoints.push_back(static_cast<uint32_t>(child));
		}
		std::string problem;
		if (outJoints.size() < 2)
			problem = fmt::format("joint '{}' has no child to swing toward", chain.Joint);
		else if (chain.ChainLength != 0 && outJoints.size() <= length)
			problem = fmt::format("joint '{}' has fewer than {} joints in a line below it", chain.Joint, length);
		else if (const auto fixed = std::ranges::find_if(outJoints.begin(), outJoints.end() - 1, [&](uint32_t joint) { return source.Nodes[joint].HasMatrix; }); fixed != outJoints.end() - 1)
			problem = fmt::format("joint '{}' has a fixed matrix and cannot be turned", source.Nodes[*fixed].Name);
		if (!problem.empty())
			outJoints.clear();
		return problem;
	}

	IKSolveResult ApplySpringChain(const MeshSource& source, AnimationLocalPose& pose, std::vector<glm::mat4>& globals, std::span<const uint32_t> joints, std::span<const glm::vec3> targets, float weight)
	{
		BS_CORE_ASSERT(joints.size() == targets.size() + 1, "one target per turned joint");
		weight = std::isfinite(weight) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
		if (weight <= 0 || targets.empty())
			return IKSolveResult::Unchanged;
		const std::span<const uint32_t> turned = joints.first(targets.size());
		if (std::ranges::any_of(turned, [&](uint32_t joint) { return source.Nodes[joint].HasMatrix; }))
			return IKSolveResult::FixedMatrix;
		if (!std::ranges::all_of(targets, [](const glm::vec3& target) { return Math::IsFinite(target); }))
			return IKSolveResult::NonFinite;
		std::vector<glm::quat> saved;
		saved.reserve(turned.size());
		for (uint32_t joint : turned)
			saved.push_back(pose.Nodes[joint].Rotation);
		const auto restore = [&] {
			for (size_t i = 0; i < turned.size(); i++)
				pose.Nodes[turned[i]].Rotation = saved[i];
			// The saved pose composed on entry, so this cannot fail.
			ComposeAnimationPose(source, pose, globals);
		};
		bool changed = false;
		for (size_t i = 0; i < turned.size(); i++)
		{
			const glm::vec3 origin = glm::vec3(globals[joints[i]][3]);
			const glm::vec3 bone = glm::vec3(globals[joints[i + 1]][3]) - origin;
			const glm::vec3 aim = targets[i] - origin;
			if (glm::length(bone) < Epsilon || glm::length(aim) < Epsilon)
				continue;
			RotateJoint(source, pose, globals, joints[i], RotationBetween(glm::normalize(bone), glm::normalize(aim)));
			// The joints below aim from where this turn puts them.
			if (!ComposeAnimationPose(source, pose, globals))
			{
				restore();
				return IKSolveResult::NonFinite;
			}
			changed = true;
		}
		if (!changed)
			return IKSolveResult::Unchanged;
		if (weight < 1.0f)
		{
			for (size_t i = 0; i < turned.size(); i++)
			{
				auto& rotation = pose.Nodes[turned[i]].Rotation;
				rotation = glm::normalize(glm::slerp(saved[i], rotation, weight));
			}
			if (!ComposeAnimationPose(source, pose, globals))
			{
				restore();
				return IKSolveResult::NonFinite;
			}
		}
		return IKSolveResult::Solved;
	}

}
