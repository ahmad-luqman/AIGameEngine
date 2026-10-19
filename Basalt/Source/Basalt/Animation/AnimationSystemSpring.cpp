#include "Basalt/Animation/AnimationSystem.h"

#include "Basalt/Animation/InverseKinematics.h"
#include "Basalt/Animation/SpringBones.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/Profiler.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <algorithm>

// The spring bone pass of AnimationSystem. It runs in LateUpdate right after inverse kinematics, so springs hang from
// the solved pose and attachments follow them.

namespace Basalt {

	namespace {
		constexpr const char* Owner = "SpringBone";

		// The average length of a transform's axes: how much it scales distances (exact for uniform scale).
		float AverageScale(const glm::mat4& transform)
		{
			return (glm::length(glm::vec3(transform[0])) + glm::length(glm::vec3(transform[1])) + glm::length(glm::vec3(transform[2]))) / 3.0f;
		}
	}

	void AnimationSystem::OnSpringDestroy(entt::registry&, entt::entity handle)
	{
		std::erase_if(m_Springs, [handle](const auto& entry) { return entry.second.Handle == handle; });
		// The pose returns to the animation (and IK) alone.
		for (auto& [id, playback] : m_Playback)
		{
			if (playback.Handle == handle)
				DropOverlay(playback, playback.SpringOverlay);
		}
	}

	AnimationSystem::SpringState& AnimationSystem::SyncSprings(Entity entity, const SpringBoneComponent& component)
	{
		SpringState& state = m_Springs[entity.GetUUID()];
		if (state.Handle != static_cast<entt::entity>(entity))
		{
			state = {};
			state.Handle = entity;
		}
		state.Chains = MatchByName(state.Chains, component.Chains, [](const SpringChain& config) { return SpringChainState{ .Name = config.Name }; });
		return state;
	}

	void AnimationSystem::SolveSprings(float seconds)
	{
		BS_PROFILE_SCOPE("Spring bones");
		for (auto handle : m_Scene.GetAllEntitiesWith<SpringBoneComponent>())
		{
			Entity entity(handle, &m_Scene);
			const auto& component = entity.GetComponent<SpringBoneComponent>();
			SpringState& state = SyncSprings(entity, component);
			if (auto* playback = Ensure(entity))
			{
				SolveEntitySprings(entity, *playback, component, state, seconds);
				continue;
			}
			// The points restart on the animated pose once the Animator can play again.
			for (SpringChainState& chain : state.Chains)
				chain.Sim.reset();
			// Ensure already warns about an empty Mesh.
			if (const auto* animator = entity.TryGetComponent<AnimatorComponent>(); !animator || !animator->Mesh.empty())
				ReportProblem(state.Problem, "not solved: " + DescribeUnplayable(entity), entity, Owner);
		}
	}

	void AnimationSystem::ResolveSpringColliders(const Ref<MeshSource>& source, const SpringBoneComponent& component, SpringState& state, const glm::mat4& world, const std::vector<glm::mat4>& globals, std::vector<SpringCollisionShape>& out, std::vector<std::string>& problems)
	{
		const bool resolved = state.ColliderSource == source && std::ranges::equal(state.ColliderJoints, component.Colliders, {}, {}, &SpringCollider::Joint);
		if (!resolved)
		{
			state.ColliderSource = source;
			state.ColliderJoints.clear();
			state.ColliderNodes.clear();
			for (const SpringCollider& collider : component.Colliders)
			{
				state.ColliderJoints.push_back(collider.Joint);
				const int32_t node = collider.Joint.empty() ? -1 : FindNode(*source, collider.Joint);
				state.ColliderNodes.push_back(collider.Joint.empty() || node >= 0 ? node : NoNode);
			}
		}
		out.clear();
		for (size_t index = 0; index < component.Colliders.size(); index++)
		{
			const SpringCollider& collider = component.Colliders[index];
			const int32_t node = state.ColliderNodes[index];
			if (node == NoNode)
			{
				problems.push_back(fmt::format("Colliders[{}]: joint '{}' not found in '{}'", index, collider.Joint, source->Key));
				continue;
			}
			if (!Math::IsFinite(collider.Offset) || !std::isfinite(collider.Radius) || !std::isfinite(collider.HalfHeight) || collider.Radius < 0 || collider.HalfHeight < 0)
			{
				problems.push_back(fmt::format("Colliders[{}]: Offset, Radius and HalfHeight must be finite and Radius and HalfHeight nonnegative; ignoring it", index));
				continue;
			}
			const glm::mat4 transform = node >= 0 ? world * globals[node] : world;
			const glm::vec3 axis(0, collider.HalfHeight, 0);
			out.push_back({ .A = glm::vec3(transform * glm::vec4(collider.Offset - axis, 1)), .B = glm::vec3(transform * glm::vec4(collider.Offset + axis, 1)), .Radius = collider.Radius * AverageScale(transform) });
		}
	}

	void AnimationSystem::SolveEntitySprings(Entity entity, Playback& playback, const SpringBoneComponent& component, SpringState& state, float seconds)
	{
		BS_CORE_ASSERT(state.Chains.size() == component.Chains.size(), "SyncSprings builds one state per chain");
		const MeshSource& source = *playback.Source;
		std::vector<std::string> problems;
		const auto report = [&] { ReportProblem(state.Problem, JoinProblems(problems), entity, Owner); };
		// Nothing could be simulated: the pose returns to the animation (and IK) alone, and the chains restart on it once
		// they can solve again rather than from points the entity left behind meanwhile.
		const auto skip = [&] {
			DropOverlay(playback, playback.SpringOverlay);
			for (SpringChainState& chain : state.Chains)
				chain.Sim.reset();
			report();
		};

		// From the layered pose with this frame's IK, never from the last spring solution, so a held pose does not
		// accumulate it.
		AnimationLocalPose pose = playback.Pose;
		for (const auto& [node, transform] : playback.IKOverlay)
			pose.Nodes[node] = transform;
		std::vector<glm::mat4>& globals = m_OverlayGlobals;
		if (!ComposeAnimationPose(source, pose, globals))
		{
			problems.emplace_back("the animated pose is not finite");
			return skip();
		}
		const glm::mat4 world = m_Scene.GetWorldTransform(entity);
		// A zero scale (e.g. a spawn effect growing the entity) has nothing to swing; that is no problem.
		if (glm::length(glm::vec3(world[0])) == 0 || glm::length(glm::vec3(world[1])) == 0 || glm::length(glm::vec3(world[2])) == 0)
			return skip();
		const glm::mat4 model = glm::inverse(world);
		if (!Math::IsFinite(model))
		{
			problems.emplace_back("the entity's transform is not invertible (degenerate or not finite)");
			return skip();
		}
		const float entityScale = AverageScale(world);

		glm::vec3 wind = component.Wind;
		if (!Math::IsFinite(wind))
		{
			problems.emplace_back("Wind is not finite; ignoring it");
			wind = glm::vec3(0);
		}
		const PhysicsWorld* physics = m_Scene.GetPhysicsWorld();
		glm::vec3 gravity = physics ? physics->GetGravity() : m_Scene.GetPhysicsSettings().Gravity;
		if (!Math::IsFinite(gravity))
		{
			problems.emplace_back("the physics gravity is not finite; ignoring it");
			gravity = glm::vec3(0);
		}
		std::vector<SpringCollisionShape>& colliders = m_SpringColliders;
		ResolveSpringColliders(playback.Source, component, state, world, globals, colliders, problems);

		// A move of a chain's first joint (with its entity) faster than TeleportSpeed restarts the chain on the animated
		// pose instead of swinging it across the jump; as in MatchVelocity, physics moves bodies at most once per fixed
		// step.
		const double window = std::max(static_cast<double>(seconds), static_cast<double>(m_Scene.GetFixedTimestep()));
		std::vector<uint32_t>& touched = m_SpringTouched;
		std::vector<glm::vec3>& animated = m_SpringAnimated;
		std::vector<glm::vec3>& targets = m_SpringTargets;
		touched.clear();
		for (size_t index = 0; index < component.Chains.size(); index++)
		{
			const SpringChain& config = component.Chains[index];
			// SyncSprings built one state per configuration, in order.
			SpringChainState& chain = state.Chains[index];
			std::vector<std::string> chainProblems;
			const float stiffness = ClampSetting(config.Stiffness, 0, SpringChain::MaxStiffness, "Stiffness", chainProblems);
			const float damping = ClampSetting(config.Damping, 0, SpringChain::MaxDamping, "Damping", chainProblems);
			const float gravityFactor = ClampSetting(config.GravityFactor, -SpringChain::MaxGravityFactor, SpringChain::MaxGravityFactor, "GravityFactor", chainProblems);
			const float radius = ClampSetting(config.Radius, 0, SpringChain::MaxRadius, "Radius", chainProblems);
			const float weight = ClampSetting(config.Weight, 0, 1, "Weight", chainProblems);
			const auto reportChain = [&] { ReportProblem(chain.Problem, JoinProblems(chainProblems), entity, Owner, &config.Name); };
			if (chain.ResolvedSource != playback.Source || chain.ResolvedJoint != config.Joint || chain.ResolvedLength != config.ChainLength)
			{
				chain.ResolveProblem = ResolveSpringChain(source, config, chain.Joints);
				chain.ResolvedSource = playback.Source;
				chain.ResolvedJoint = config.Joint;
				chain.ResolvedLength = config.ChainLength;
				chain.Sim.reset();
			}
			if (!chain.ResolveProblem.empty())
			{
				chainProblems.push_back(chain.ResolveProblem);
				reportChain();
				continue;
			}
			const std::vector<uint32_t>& joints = chain.Joints;
			animated.clear();
			for (uint32_t joint : joints)
				animated.emplace_back(world * globals[joint][3]);
			if (chain.Sim && glm::length(animated.front() - chain.Sim->Animated.front()) > TeleportSpeed * window)
				chain.Sim.reset();
			if (!chain.Sim)
			{
				auto& sim = chain.Sim.emplace();
				for (size_t i = 1; i < joints.size(); i++)
					sim.Points.push_back({ .Position = animated[i] });
			}
			else
				SimulateSpringChain({ .Animated = animated, .PreviousAnimated = chain.Sim->Animated, .Colliders = colliders, .Stiffness = stiffness, .Damping = damping, .Radius = radius * entityScale, .Acceleration = gravity * gravityFactor + wind, .Seconds = seconds }, chain.Sim->Points);
			chain.Sim->Animated = animated;
			targets.clear();
			for (const SpringPoint& point : chain.Sim->Points)
				targets.emplace_back(model * glm::vec4(point.Position, 1.0f));
			switch (const IKSolveResult result = SolveAimChainIK(source, pose, globals, { .Joints = joints, .Targets = targets, .Weight = weight }))
			{
				case IKSolveResult::Solved:
					touched.insert(touched.end(), joints.begin(), joints.end() - 1);
					break;
				// Weight 0: the points keep simulating, so a returning weight picks the swing up where it is.
				case IKSolveResult::Unchanged:
					break;
				case IKSolveResult::Degenerate:
					chainProblems.emplace_back("its bones have zero length (or every point sits on its joint), so nothing can swing");
					break;
				case IKSolveResult::NonFinite:
					chainProblems.emplace_back("the simulated points or the swung pose are not finite; restarting the chain");
					// Restart from the animated pose rather than from points that could not be shown.
					chain.Sim.reset();
					break;
				case IKSolveResult::FixedMatrix:
					// ResolveSpringChain rejects fixed-matrix joints; kept for completeness.
					chainProblems.emplace_back(DescribeIKResult(result));
					break;
			}
			reportChain();
		}
		if (touched.empty())
		{
			// No chain turned (weights 0, or problems reported per chain): the simulation carries on underneath.
			DropOverlay(playback, playback.SpringOverlay);
			return report();
		}
		// SolveAimChainIK leaves globals composed (finite); skins add their inverse binds.
		if (!SkinPalettesFinite(source, globals))
		{
			problems.emplace_back("the swung pose is not finite");
			return skip();
		}
		std::ranges::sort(touched);
		touched.erase(std::ranges::unique(touched).begin(), touched.end());
		StoreOverlay(playback, playback.SpringOverlay, touched, pose, globals);
		report();
	}

	bool AnimationSystem::ResetSpringBones(Entity entity)
	{
		const auto* component = entity ? entity.TryGetComponent<SpringBoneComponent>() : nullptr;
		if (!component)
			return false;
		for (SpringChainState& chain : SyncSprings(entity, *component).Chains)
			chain.Sim.reset();
		return true;
	}

}
