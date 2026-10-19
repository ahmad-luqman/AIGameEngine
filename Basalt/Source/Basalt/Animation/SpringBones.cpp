#include "Basalt/Animation/SpringBones.h"

#include "Basalt/Animation/InverseKinematics.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Scene/Components.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace Basalt {

	// Explicit integration of the pull is stable while stiffness * substep² stays below 4; keep a wide margin.
	static_assert(SpringChain::MaxStiffness / (SpringStepRate * SpringStepRate) < 1.0f, "the stiffest spring must stay stable at the longest substep");

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

		// `point` pushed out of the shape grown by `radius`, or nullopt when it is outside already. A point on the shape's
		// axis leaves across `bone` (its bone's direction), which keeps it near its bone length.
		std::optional<glm::vec3> PushOut(const SpringCollisionShape& shape, const glm::vec3& point, float radius, const glm::vec3& bone)
		{
			const glm::vec3 axis = shape.B - shape.A;
			const float lengthSquared = glm::dot(axis, axis);
			const float t = lengthSquared > 0 ? std::clamp(glm::dot(point - shape.A, axis) / lengthSquared, 0.0f, 1.0f) : 0.0f;
			const glm::vec3 closest = shape.A + axis * t;
			const float reach = shape.Radius + radius;
			glm::vec3 offset = point - closest;
			if (glm::length(offset) >= reach)
				return std::nullopt;
			if (glm::length(offset) < Epsilon)
			{
				// Across both the bone and a capsule's axis; failing that (parallel or vanishing), any direction across
				// whichever of the two is there.
				const glm::vec3 along = lengthSquared > 0 ? axis : bone;
				offset = lengthSquared > 0 ? glm::cross(axis, bone) : glm::cross(bone, glm::vec3(0, 1, 0));
				for (const glm::vec3 fallback : { glm::vec3(0, 1, 0), glm::vec3(1, 0, 0) })
					if (glm::length(offset) < Epsilon)
						offset = glm::cross(along, fallback);
				if (glm::length(offset) < Epsilon)
					offset = glm::vec3(0, 1, 0);
			}
			return closest + glm::normalize(offset) * reach;
		}
	}

	void SimulateSpringChain(const SpringSimulationRequest& request, std::span<SpringPoint> points)
	{
		const size_t count = points.size();
		BS_CORE_ASSERT(request.Animated.size() == count + 1 && request.PreviousAnimated.size() == count + 1, "one animated position per joint plus the tip");
		BS_CORE_ASSERT(count <= SpringChain::MaxChainLength, "chains have at most MaxChainLength joints");
		BS_CORE_ASSERT(request.Stiffness >= 0 && request.Damping >= 0, "a negative stiffness or damping adds energy");
		constexpr float longest = static_cast<float>(MaxSpringSteps) / SpringStepRate;
		if (count == 0 || !(request.Seconds > 0))
			return;
		const float seconds = std::min(request.Seconds, longest);
		// A hitch: the points move along with the first joint for the part of the frame that is not simulated, and the
		// animated pose is interpolated over the simulated rest.
		const float skipped = 1.0f - seconds / request.Seconds;
		std::array<glm::vec3, SpringChain::MaxChainLength + 1> start{};
		for (size_t i = 0; i <= count; i++)
			start[i] = glm::mix(request.PreviousAnimated[i], request.Animated[i], skipped);
		const glm::vec3 carried = start[0] - request.PreviousAnimated[0];
		for (SpringPoint& point : points)
			point.Position += carried;

		const uint32_t steps = std::clamp(static_cast<uint32_t>(std::ceil(seconds * SpringStepRate)), 1u, MaxSpringSteps);
		const float step = seconds / static_cast<float>(steps);
		const float decay = std::exp(-request.Damping * step);
		std::array<glm::vec3, SpringChain::MaxChainLength + 1> animated{};
		for (uint32_t substep = 1; substep <= steps; substep++)
		{
			// The animated pose moves evenly through the frame, so a fast-moving character does not jerk its points
			// once per frame.
			const float progress = static_cast<float>(substep) / static_cast<float>(steps);
			for (size_t i = 0; i <= count; i++)
				animated[i] = glm::mix(start[i], request.Animated[i], progress);
			glm::vec3 head = animated[0];
			// How far the simulated chain so far turned away from the animated one: the animated shape below follows it.
			glm::quat turn(1, 0, 0, 0);
			for (size_t i = 0; i < count; i++)
			{
				SpringPoint& point = points[i];
				const glm::vec3 bone = animated[i + 1] - animated[i];
				const float length = glm::length(bone);
				const glm::vec3 target = head + turn * bone;
				// Semi-implicit Euler: the velocity first, so the result does not depend on how long earlier substeps were.
				point.Velocity = point.Velocity * decay + (request.Stiffness * (target - point.Position) + request.Acceleration) * step;
				glm::vec3 next = KeepLength(head, point.Position + point.Velocity * step, length, target);
				for (uint32_t iteration = 0; iteration < SpringConstraintIterations; iteration++)
				{
					bool pushed = false;
					for (const SpringCollisionShape& shape : request.Colliders)
						if (const auto out = PushOut(shape, next, request.Radius, next - head))
						{
							next = *out;
							pushed = true;
						}
					if (!pushed)
						break;
					next = KeepLength(head, next, length, target);
				}
				// The constraints' corrections become velocity, as in position-based dynamics.
				point.Velocity = (next - point.Position) / step;
				point.Position = next;
				const glm::vec3 swung = next - head;
				if (length > Epsilon && glm::length(swung) > Epsilon)
					turn = RotationBetween(bone / length, glm::normalize(swung));
				head = next;
			}
		}
	}

	int32_t FirstChild(const MeshSource& source, uint32_t node)
	{
		const auto found = std::ranges::find(source.Nodes, static_cast<int32_t>(node), &MeshNode::Parent);
		return found == source.Nodes.end() ? -1 : static_cast<int32_t>(found - source.Nodes.begin());
	}

	size_t ChildCount(const MeshSource& source, uint32_t node)
	{
		return static_cast<size_t>(std::ranges::count(source.Nodes, static_cast<int32_t>(node), &MeshNode::Parent));
	}

	std::string ResolveSpringChain(const MeshSource& source, const SpringChain& chain, std::vector<uint32_t>& outJoints)
	{
		outJoints.clear();
		const auto fail = [&](std::string problem) {
			outJoints.clear();
			return problem;
		};
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
		if (outJoints.size() < 2)
			return fail(fmt::format("joint '{}' has no child to swing toward", chain.Joint));
		if (chain.ChainLength != 0 && outJoints.size() <= length)
			return fail(fmt::format("joint '{}' needs {} joints and a tip in a line from it", chain.Joint, length));
		for (uint32_t joint : std::span(outJoints).first(outJoints.size() - 1))
			if (source.Nodes[joint].HasMatrix)
				return fail(fmt::format("joint '{}' has a fixed matrix and cannot be turned", source.Nodes[joint].Name));
		return {};
	}

}
