#include "Basalt/Animation/AnimationSystem.h"

#include "Basalt/Animation/SpringBones.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/Profiler.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <set>

// The spring bone pass of AnimationSystem. It runs in LateUpdate right after inverse kinematics, so springs hang from
// the solved pose and attachments follow them.

namespace Basalt {

	namespace {
		// Logs `problem` when it changed since `reported`; `chain` names the chain it is about.
		void Report(std::string& reported, std::string problem, Entity entity, const SpringChain* chain = nullptr)
		{
			if (!problem.empty() && problem != reported)
			{
				if (chain)
					BS_CORE_WARN("SpringBone '{}': chain '{}': {}", entity.GetName(), chain->Name, problem);
				else
					BS_CORE_WARN("SpringBone '{}': {}", entity.GetName(), problem);
			}
			reported = std::move(problem);
		}

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
			if (playback.Handle == handle && !playback.SpringOverlay.empty())
			{
				playback.SpringOverlay.clear();
				Finish(playback);
			}
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
		std::vector<SpringChainState> chains;
		chains.reserve(component.Chains.size());
		// Each state moves to at most one configuration, even if C++ code wrote duplicate names past Validate.
		std::vector<bool> taken(state.Chains.size(), false);
		for (const SpringChain& config : component.Chains)
		{
			SpringChainState chain;
			const auto found = std::ranges::find_if(state.Chains, [&](const SpringChainState& old) { return !taken[&old - state.Chains.data()] && old.Name == config.Name; });
			if (found != state.Chains.end())
			{
				taken[static_cast<size_t>(found - state.Chains.begin())] = true;
				chain = std::move(*found);
			}
			else
				chain.Name = config.Name;
			chains.push_back(std::move(chain));
		}
		state.Chains = std::move(chains);
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
				SolveEntitySprings(entity, *playback, component, state, seconds);
			// Ensure already warns about an empty Mesh.
			else if (const auto* animator = entity.TryGetComponent<AnimatorComponent>(); !animator || !animator->Mesh.empty())
				Report(state.Problem, "not solved: " + DescribeUnplayable(entity), entity);
		}
	}

	void AnimationSystem::SolveEntitySprings(Entity entity, Playback& playback, const SpringBoneComponent& component, SpringState& state, float seconds)
	{
		BS_CORE_ASSERT(state.Chains.size() == component.Chains.size(), "SyncSprings builds one state per chain");
		const MeshSource& source = *playback.Source;
		std::vector<std::string> problems;
		const auto finish = [&](bool solved) {
			// Without a solution the pose returns to the animation (and IK) alone.
			if (!solved && !playback.SpringOverlay.empty())
			{
				playback.SpringOverlay.clear();
				Finish(playback);
			}
			Report(state.Problem, fmt::format("{}", fmt::join(problems, "; ")), entity);
		};

		// From the layered pose with this frame's IK, never from the last spring solution, so a held pose does not
		// accumulate it.
		AnimationLocalPose pose = playback.Pose;
		for (const auto& [node, transform] : playback.IKOverlay)
			pose.Nodes[node] = transform;
		std::vector<glm::mat4>& globals = m_IKGlobals;
		if (!ComposeAnimationPose(source, pose, globals))
		{
			problems.emplace_back("the animated pose is not finite");
			return finish(false);
		}
		const glm::mat4 world = m_Scene.GetWorldTransform(entity);
		// A zero scale (e.g. a spawn effect growing the entity) has nothing to swing; that is no problem.
		if (glm::length(glm::vec3(world[0])) == 0 || glm::length(glm::vec3(world[1])) == 0 || glm::length(glm::vec3(world[2])) == 0)
			return finish(false);
		const glm::mat4 model = glm::inverse(world);
		if (!Math::IsFinite(model))
		{
			problems.emplace_back("the entity's transform is not invertible (degenerate or not finite)");
			return finish(false);
		}
		const float entityScale = AverageScale(world);

		glm::vec3 wind = component.Wind;
		if (!Math::IsFinite(wind))
		{
			problems.emplace_back("Wind is not finite; ignoring it");
			wind = glm::vec3(0);
		}
		const PhysicsWorld* physics = m_Scene.GetPhysicsWorld();
		const glm::vec3 gravity = physics ? physics->GetGravity() : m_Scene.GetPhysicsSettings().Gravity;

		std::vector<SpringCollisionShape> colliders;
		colliders.reserve(component.Colliders.size());
		for (size_t index = 0; index < component.Colliders.size(); index++)
		{
			const SpringCollider& collider = component.Colliders[index];
			glm::mat4 transform = world;
			if (!collider.Joint.empty())
			{
				const int32_t joint = FindNode(source, collider.Joint);
				if (joint < 0)
				{
					problems.push_back(fmt::format("Colliders[{}]: joint '{}' not found in '{}'", index, collider.Joint, source.Key));
					continue;
				}
				transform = world * globals[joint];
			}
			if (!Math::IsFinite(collider.Offset) || !std::isfinite(collider.Radius) || !std::isfinite(collider.HalfHeight) || collider.Radius < 0 || collider.HalfHeight < 0)
			{
				problems.push_back(fmt::format("Colliders[{}]: Offset, Radius and HalfHeight must be finite and Radius and HalfHeight nonnegative; ignoring it", index));
				continue;
			}
			const glm::vec3 axis(0, collider.HalfHeight, 0);
			colliders.push_back({ .A = glm::vec3(transform * glm::vec4(collider.Offset - axis, 1)), .B = glm::vec3(transform * glm::vec4(collider.Offset + axis, 1)), .Radius = collider.Radius * AverageScale(transform) });
		}

		// A move of the entity faster than TeleportSpeed restarts its chains on the animated pose instead of swinging
		// them across the jump; as in MatchVelocity, physics moves bodies at most once per fixed step.
		const double window = std::max(static_cast<double>(seconds), static_cast<double>(m_Scene.GetFixedTimestep()));
		std::set<uint32_t> touched;
		std::vector<uint32_t> joints;
		std::vector<glm::vec3> animated;
		std::vector<glm::vec3> targets;
		for (size_t index = 0; index < component.Chains.size(); index++)
		{
			const SpringChain& config = component.Chains[index];
			// SyncSprings built one state per configuration, in order.
			SpringChainState& chain = state.Chains[index];
			std::vector<std::string> chainProblems;
			const float stiffness = ClampSetting(config.Stiffness, 0, 10000, "Stiffness", chainProblems);
			const float damping = ClampSetting(config.Damping, 0, 1000, "Damping", chainProblems);
			const float gravityFactor = ClampSetting(config.GravityFactor, -100, 100, "GravityFactor", chainProblems);
			const float radius = ClampSetting(config.Radius, 0, 100, "Radius", chainProblems);
			const float weight = ClampSetting(config.Weight, 0, 1, "Weight", chainProblems);
			const auto report = [&] { Report(chain.Problem, fmt::format("{}", fmt::join(chainProblems, "; ")), entity, &config); };
			if (std::string problem = ResolveSpringChain(source, config, joints); !problem.empty())
			{
				chainProblems.push_back(std::move(problem));
				chain.Joints.clear();
				chain.Initialized = false;
				report();
				continue;
			}
			animated.clear();
			for (uint32_t joint : joints)
				animated.emplace_back(world * globals[joint][3]);
			const bool teleported = chain.Initialized && glm::length(animated.front() - chain.Animated.front()) > TeleportSpeed * window;
			if (!chain.Initialized || teleported || chain.Joints != joints)
			{
				chain.Joints = joints;
				chain.Points.resize(joints.size() - 1);
				for (size_t i = 0; i < chain.Points.size(); i++)
					chain.Points[i] = { .Position = animated[i + 1], .Previous = animated[i + 1] };
				chain.Initialized = true;
			}
			else
			{
				SimulateSpringChain({ .Animated = animated, .PreviousAnimated = chain.Animated, .Colliders = colliders, .Stiffness = stiffness, .Damping = damping, .Radius = radius * entityScale, .Acceleration = gravity * gravityFactor + wind, .Seconds = seconds }, chain.Points);
			}
			chain.Animated = animated;
			targets.clear();
			for (const SpringPoint& point : chain.Points)
				targets.emplace_back(model * glm::vec4(point.Position, 1.0f));
			const IKSolveResult result = ApplySpringChain(source, pose, globals, joints, targets, weight);
			if (result == IKSolveResult::Solved)
				touched.insert(joints.begin(), joints.end() - 1);
			else if (result != IKSolveResult::Unchanged)
			{
				chainProblems.emplace_back(DescribeIKResult(result));
				// Restart from the animated pose rather than from points that could not be shown.
				chain.Initialized = false;
			}
			report();
		}
		if (touched.empty())
			return finish(false);
		// ApplySpringChain leaves globals composed (finite); skins add their inverse binds.
		if (!SkinPalettesFinite(source, globals))
		{
			problems.emplace_back("the swung pose is not finite");
			for (SpringChainState& chain : state.Chains)
				chain.Initialized = false;
			return finish(false);
		}
		playback.SpringOverlay.clear();
		for (uint32_t node : touched)
			playback.SpringOverlay.push_back({ node, pose.Nodes[node] });
		// The same as Finish would compose (Pose with both overlays), without sampling the layers again.
		playback.Globals = globals;
		playback.OverlayDropped = false;
		finish(true);
	}

	bool AnimationSystem::ResetSpringBones(Entity entity)
	{
		const auto* component = entity ? entity.TryGetComponent<SpringBoneComponent>() : nullptr;
		if (!component)
			return false;
		for (SpringChainState& chain : SyncSprings(entity, *component).Chains)
			chain.Initialized = false;
		return true;
	}

}
