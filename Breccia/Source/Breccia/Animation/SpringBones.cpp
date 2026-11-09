#include "Breccia/Animation/SpringBones.h"

#include "Breccia/Animation/InverseKinematics.h"
#include "Breccia/Core/Assert.h"
#include "Breccia/Scene/Components.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace Breccia {

	// Semi-implicit Euler integration of the pull is stable while stiffness * substep² stays below 4; keep a wide margin.
	static_assert(SpringChain::MaxStiffness / (SpringStepRate * SpringStepRate) < 1.0f, "the stiffest spring must stay stable at the longest substep");

	namespace {
		constexpr float Epsilon = 1e-6f;
		// How close a swept sphere comes to a surface: above the rounding of positions at a few hundred meters.
		constexpr float SpringContactGap = 1e-4f;

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

		// `candidate`, or (when it vanishes) a direction across `along`, unnormalized.
		glm::vec3 AcrossIfVanished(glm::vec3 candidate, const glm::vec3& along)
		{
			for (const glm::vec3 fallback : { glm::vec3(0, 1, 0), glm::vec3(1, 0, 0) })
				if (glm::length(candidate) < Epsilon)
					candidate = glm::cross(along, fallback);
			return candidate;
		}

		// The point of the shape's axis (its segment A..B) nearest `point`.
		glm::vec3 ClosestOnAxis(const SpringCollisionShape& shape, const glm::vec3& point)
		{
			const glm::vec3 axis = shape.B - shape.A;
			const float lengthSquared = glm::dot(axis, axis);
			const float t = lengthSquared > 0 ? std::clamp(glm::dot(point - shape.A, axis) / lengthSquared, 0.0f, 1.0f) : 0.0f;
			return shape.A + axis * t;
		}

		// `point` pushed out of the shape grown by `radius`, or nullopt when it is outside already. A point on the shape's
		// axis leaves across `bone` (its bone's direction), which keeps it near its bone length.
		std::optional<glm::vec3> PushOut(const SpringCollisionShape& shape, const glm::vec3& point, float radius, const glm::vec3& bone)
		{
			const glm::vec3 closest = ClosestOnAxis(shape, point);
			const float reach = shape.Radius + radius;
			glm::vec3 offset = point - closest;
			const float distance = glm::length(offset);
			if (distance >= reach)
				return std::nullopt;
			if (distance < Epsilon)
			{
				const glm::vec3 axis = shape.B - shape.A;
				const float lengthSquared = glm::dot(axis, axis);
				// Across both the bone and a capsule's axis; failing that (parallel or vanishing), any direction across
				// whichever of the two is there.
				offset = AcrossIfVanished(lengthSquared > 0 ? glm::cross(axis, bone) : glm::vec3(0), lengthSquared > 0 ? axis : bone);
				if (glm::length(offset) < Epsilon)
					offset = glm::vec3(0, 1, 0);
			}
			return closest + glm::normalize(offset) * reach;
		}

		// Where a sphere of `radius` moving from `from` to `to` first touches the shape, by sphere tracing: the distance to
		// the surface is a step that cannot overshoot it, however thin the shape. Like the world's sweeps it does not
		// stop a sphere that starts inside and moves out. A sphere still grazing after the last step counts as clear: it ends
		// where the constraints put it, and the next substep's PushOut takes care of any overlap.
		std::optional<SpringSweepHit> SweepShape(const SpringCollisionShape& shape, float radius, const glm::vec3& from, const glm::vec3& to)
		{
			constexpr uint32_t Steps = 32;
			const float distance = glm::length(to - from);
			if (distance < Epsilon)
				return std::nullopt;
			const glm::vec3 direction = (to - from) / distance;
			const float reach = shape.Radius + radius;
			float travelled = 0;
			for (uint32_t i = 0; i < Steps && travelled <= distance; i++)
			{
				const glm::vec3 point = from + direction * travelled;
				const glm::vec3 offset = point - ClosestOnAxis(shape, point);
				const float separation = glm::length(offset);
				const float gap = separation - reach;
				if (gap <= SpringContactGap)
				{
					const glm::vec3 normal = separation < Epsilon ? -direction : offset / separation;
					if (glm::dot(direction, normal) >= 0)
						return std::nullopt;
					return SpringSweepHit{ .Fraction = travelled / distance, .Normal = normal };
				}
				travelled += gap;
			}
			return std::nullopt;
		}

		// The nearest of what the colliders and the world put between the two positions.
		std::optional<SpringSweepHit> FirstHit(const SpringSimulationRequest& request, const glm::vec3& from, const glm::vec3& to)
		{
			std::optional<SpringSweepHit> first = request.WorldSweep ? request.WorldSweep(from, to) : std::nullopt;
			for (const SpringCollisionShape& shape : request.Colliders)
				if (const auto hit = SweepShape(shape, request.Radius, from, to); hit && (!first || hit->Fraction < first->Fraction))
					first = hit;
			return first;
		}

		// The sphere's way from `from` toward `to`: up to what it meets, then along that surface with the rest of the move,
		// a little off it so the slide does not count as touching again.
		glm::vec3 SweepPoint(const SpringSimulationRequest& request, const glm::vec3& from, const glm::vec3& to)
		{
			glm::vec3 position = from, goal = to;
			for (uint32_t iteration = 0; iteration < SpringSweepIterations; iteration++)
			{
				const auto hit = FirstHit(request, position, goal);
				if (!hit)
					return goal;
				const glm::vec3 reached = glm::mix(position, goal, hit->Fraction);
				const glm::vec3 rest = goal - reached;
				position = reached + hit->Normal * SpringContactGap;
				goal = position + rest - hit->Normal * glm::dot(rest, hit->Normal);
			}
			return position;
		}

		// `point` turned about `head` to within the angle whose cosine and sine are given of the direction `head` to `target`,
		// at its distance from head.
		glm::vec3 LimitAngle(const glm::vec3& head, const glm::vec3& point, const glm::vec3& target, float cosMax, float sinMax)
		{
			const glm::vec3 offset = point - head, axisOffset = target - head;
			const float length = glm::length(offset), axisLength = glm::length(axisOffset);
			if (length < Epsilon || axisLength < Epsilon)
				return point;
			const glm::vec3 direction = offset / length, axis = axisOffset / axisLength;
			const float cosine = glm::dot(direction, axis);
			if (cosine >= cosMax)
				return point;
			// Toward `direction` from the axis; any side for a point straight behind it.
			const glm::vec3 side = glm::normalize(AcrossIfVanished(direction - axis * cosine, axis));
			return head + (axis * cosMax + side * sinMax) * length;
		}
	}

	void SimulateSpringChain(const SpringSimulationRequest& request, std::span<SpringPoint> points)
	{
		const size_t count = points.size();
		BR_CORE_ASSERT(request.Animated.size() == count + 1 && request.PreviousAnimated.size() == count + 1, "one animated position per joint plus the tip");
		BR_CORE_ASSERT(count <= SpringChain::MaxChainLength, "chains have at most MaxChainLength joints");
		BR_CORE_ASSERT(request.Stiffness >= 0 && request.Stiffness <= SpringChain::MaxStiffness && request.Damping >= 0 && request.Radius >= 0, "settings outside their ranges add energy or overflow");
		BR_CORE_ASSERT(request.MaxAngle >= 0 && request.MaxAngle <= std::numbers::pi_v<float>, "MaxAngle is between 0 and pi");
		BR_CORE_ASSERT(!request.Continuous || !request.World || request.WorldSweep, "a world that points are kept out of must also stop them");
		constexpr float longest = static_cast<float>(MaxSpringSteps) / SpringStepRate;
		// The buffers below hold MaxChainLength joints: a longer chain (only C++ callers past the assert) is not
		// simulated.
		if (count == 0 || count > SpringChain::MaxChainLength || request.Animated.size() != count + 1 || request.PreviousAnimated.size() != count + 1 || !(request.Seconds > 0))
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
		// Below pi the limit can bind; at pi every direction is inside it.
		const bool limited = request.MaxAngle < std::numbers::pi_v<float>;
		const float cosMax = std::cos(request.MaxAngle), sinMax = std::sin(request.MaxAngle);
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
				// The velocity is stored (not the previous position, as in Verlet), so substeps of different lengths integrate
				// correctly; updating it before the position (semi-implicit Euler) keeps the stiff pull stable.
				point.Velocity = point.Velocity * decay + (request.Stiffness * (target - point.Position) + request.Acceleration) * step;
				glm::vec3 next = KeepLength(head, point.Position + point.Velocity * step, length, target);
				if (limited)
					next = LimitAngle(head, next, target, cosMax, sinMax);
				for (uint32_t iteration = 0; iteration < SpringConstraintIterations; iteration++)
				{
					bool pushed = false;
					for (const SpringCollisionShape& shape : request.Colliders)
						if (const auto out = PushOut(shape, next, request.Radius, next - head))
						{
							next = *out;
							pushed = true;
						}
					if (request.World)
						if (const auto out = request.World(next))
						{
							next = *out;
							pushed = true;
						}
					if (!pushed)
						break;
					next = KeepLength(head, next, length, target);
				}
				// Last: keeping the bone's length can carry a point across what the sweep stopped it at.
				if (request.Continuous)
					if (const glm::vec3 swept = SweepPoint(request, point.Position, next); glm::length(swept - head) <= length * SpringSweepStretch)
						next = swept;
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

	bool NamesSpringColliders(const SpringChain& chain)
	{
		return std::ranges::any_of(chain.Colliders, [](const SpringChainCollider& named) { return !named.Name.empty(); });
	}

	bool SpringChainNames(const SpringChain& chain, const std::string& name)
	{
		return !name.empty() && std::ranges::find(chain.Colliders, name, &SpringChainCollider::Name) != chain.Colliders.end();
	}

	bool NamesMissingSpringCollider(const SpringChainCollider& named, std::span<const SpringCollider> colliders)
	{
		return !named.Name.empty() && std::ranges::find(colliders, named.Name, &SpringCollider::Name) == colliders.end();
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
