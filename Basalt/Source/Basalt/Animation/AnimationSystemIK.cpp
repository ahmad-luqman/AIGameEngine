#include "Basalt/Animation/AnimationSystem.h"

#include "Basalt/Animation/InverseKinematics.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/Profiler.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

// The inverse kinematics pass of AnimationSystem. It runs in LateUpdate: after physics and OnLateUpdate in a scene, at
// the end of Update for standalone callers.

namespace Basalt {

	namespace {
		// A chain resolved against the model for one solve.
		struct ResolvedChain
		{
			const IKChain* Config = nullptr;
			// Reports this chain's problem once the solve is done.
			std::string* Problem = nullptr;
			float Weight = 0;
			// TwoBone: upper, middle, end. LookAt: the aiming joint, then its ancestors.
			std::vector<uint32_t> Joints;
			// Model space.
			glm::vec3 Target = glm::vec3(0);
			std::optional<glm::quat> Rotation;
			// A ground chain whose foot already stands on flat ground at its animated height. Solving it anyway would
			// still bend a straight leg by the solver's reach margin.
			bool Settled = false;
		};

		// A component value C++ code wrote outside its valid range (Validate rejects it from data), clamped.
		float ClampSetting(float value, float low, float high, const std::string& name, std::vector<std::string>& problems)
		{
			if (std::isfinite(value) && value >= low && value <= high)
				return value;
			problems.push_back(fmt::format("{} {} must be between {} and {}; clamping", name, value, low, high));
			return std::isfinite(value) ? std::clamp(value, low, high) : low;
		}

		// Logs `problem` with `prefix` when it changed since `reported`.
		void Report(std::string& reported, std::string problem, Entity entity, const std::string& prefix)
		{
			if (!problem.empty() && problem != reported)
				BS_CORE_WARN("InverseKinematics '{}': {}{}", entity.GetName(), prefix, problem);
			reported = std::move(problem);
		}
	}

	AnimationSystem::IKState& AnimationSystem::SyncIK(Entity entity, const InverseKinematicsComponent& component)
	{
		IKState& state = m_IK[entity.GetUUID()];
		if (state.Handle != static_cast<entt::entity>(entity))
		{
			state = {};
			state.Handle = entity;
		}
		std::vector<IKChainState> chains;
		chains.reserve(component.Chains.size());
		for (const IKChain& config : component.Chains)
		{
			// Names are unique (Validate), so each state moves to at most one configuration.
			IKChainState chain;
			if (auto found = std::ranges::find(state.Chains, config.Name, &IKChainState::Name); found != state.Chains.end())
				chain = std::move(*found);
			else
			{
				chain.Name = config.Name;
				chain.Weight = FadingWeight(config.Weight);
			}
			chain.Weight.Configure(std::isfinite(config.Weight) ? std::clamp(config.Weight, 0.0f, 1.0f) : 0.0f);
			chains.push_back(std::move(chain));
		}
		state.Chains = std::move(chains);
		return state;
	}

	void AnimationSystem::SolveIK(float seconds)
	{
		BS_PROFILE_SCOPE("Inverse kinematics");
		for (auto handle : m_Scene.GetAllEntitiesWith<InverseKinematicsComponent>())
		{
			Entity entity(handle, &m_Scene);
			const auto& component = entity.GetComponent<InverseKinematicsComponent>();
			IKState& state = SyncIK(entity, component);
			for (IKChainState& chain : state.Chains)
				chain.Weight.Advance(seconds);
			if (auto* playback = Ensure(entity))
				SolveEntityIK(entity, *playback, component, state, seconds);
			else
				Report(state.Problem, "not solved: " + DescribeUnplayable(entity), entity, {});
		}
	}

	void AnimationSystem::SolveEntityIK(Entity entity, Playback& playback, const InverseKinematicsComponent& component, IKState& state, float seconds)
	{
		const MeshSource& source = *playback.Source;
		std::vector<std::string> problems;
		const auto finish = [&](bool solved) {
			// Without a solution the pose returns to the animation alone.
			if (!solved && !playback.IKOverlay.empty())
			{
				playback.IKOverlay.clear();
				Finish(playback);
			}
			Report(state.Problem, fmt::format("{}", fmt::join(problems, "; ")), entity, {});
		};

		// Always from the layered pose without IK, so held poses never accumulate last frame's solution.
		AnimationLocalPose pose = playback.Pose;
		std::vector<glm::mat4>& globals = m_IKGlobals;
		if (!ComposeAnimationPose(source, pose, globals))
		{
			problems.emplace_back("the animated pose is not finite");
			return finish(false);
		}
		const glm::mat4 world = m_Scene.GetWorldTransform(entity);
		const glm::mat4 model = glm::inverse(world);
		if (!Math::IsFinite(model))
		{
			problems.emplace_back("the entity's transform has no inverse (zero scale)");
			return finish(false);
		}
		// World vectors and rotations into the model's space; normals by the inverse transpose.
		const auto toModelRotation = [&](const glm::quat& rotation) { return GlobalRotation(model * glm::mat4_cast(rotation)); };
		const glm::vec3 up = glm::normalize(glm::vec3(world[1]));
		const glm::vec3 origin = glm::vec3(world[3]);
		const float rayHeight = ClampSetting(component.RayHeight, 0, 100, "RayHeight", problems);
		const float rayDepth = ClampSetting(component.RayDepth, 0, 100, "RayDepth", problems);
		const float smoothing = ClampSetting(component.Smoothing, 0, 100, "Smoothing", problems);

		PhysicsWorld* physics = m_Scene.GetPhysicsWorld();
		// Feet follow the ground only while the character stands on it, so a jump keeps its animated legs. The
		// character's bodies (on the entity or the controller's entity above it, and below either) are not ground;
		// other ancestors (a moving platform the character is parented to) are.
		bool airborne = false;
		Entity body = entity;
		if (physics)
			for (Entity holder = entity; holder; holder = holder.GetParent())
				if (holder.HasComponent<CharacterControllerComponent>() && physics->HasCharacter(holder))
				{
					airborne = !physics->IsCharacterGrounded(holder);
					body = holder;
					break;
				}
		const float blend = smoothing > 0 ? 1.0f - std::exp(-seconds / smoothing) : 1.0f;

		std::vector<ResolvedChain> chains;
		float pelvisOffset = 0;
		bool grounded = false;
		for (size_t index = 0; index < component.Chains.size(); index++)
		{
			const IKChain& config = component.Chains[index];
			// SyncIK built one state per configuration, in order.
			IKChainState& chain = state.Chains[index];
			const std::string prefix = fmt::format("chain '{}': ", config.Name);
			// SyncIK clamped the weight; report a value C++ code wrote out of range.
			ClampSetting(config.Weight, 0, 1, fmt::format("chain '{}' Weight", config.Name), problems);
			ResolvedChain resolved{ .Config = &config, .Problem = &chain.Problem, .Weight = chain.Weight.Get() };
			if (std::string problem = ResolveIKChain(source, config, resolved.Joints); !problem.empty())
			{
				Report(chain.Problem, std::move(problem), entity, prefix);
				continue;
			}
			if (const auto* position = std::get_if<IKChainState::PositionTarget>(&chain.Target))
			{
				resolved.Target = position->Position;
				resolved.Rotation = position->Rotation;
			}
			else if (const auto* followed = std::get_if<IKChainState::EntityTarget>(&chain.Target))
			{
				const Entity target = m_Scene.GetEntityByUUID(followed->Entity);
				if (!target)
				{
					Report(chain.Problem, "its target entity no longer exists", entity, prefix);
					continue;
				}
				const glm::mat4 transform = m_Scene.GetWorldTransform(target);
				resolved.Target = glm::vec3(transform[3]);
				if (followed->MatchRotation)
					resolved.Rotation = GlobalRotation(transform);
			}
			else if (config.Ground && config.Type == IKChainType::TwoBone)
			{
				// The ground below the animated foot, relative to the plane through the entity's origin: the animation is
				// authored on that plane, so lifts and steps keep their height above the ground.
				const uint32_t end = resolved.Joints.back();
				const glm::vec3 foot = glm::vec3(world * globals[end][3]);
				const glm::vec3 base = foot - up * glm::dot(foot - origin, up);
				float offset = 0;
				glm::vec3 normal = up;
				if (physics && !airborne && rayHeight + rayDepth > 0)
				{
					const PhysicsQueryFilter filter{ .IgnoreEntity = body.GetUUID() };
					float nearest = std::numeric_limits<float>::max();
					for (const RaycastHit& hit : physics->RaycastAll(base + up * rayHeight, -up, rayHeight + rayDepth, filter))
					{
						const Entity hitEntity = m_Scene.GetEntityByUUID(hit.EntityID);
						if (hitEntity && (hitEntity == entity || m_Scene.IsDescendantOf(hitEntity, body) || m_Scene.IsDescendantOf(hitEntity, entity)))
							continue;
						// Surfaces facing away from up (undersides, back faces) are not ground.
						if (hit.Distance < nearest && glm::dot(hit.Normal, up) > 0)
						{
							nearest = hit.Distance;
							offset = rayHeight - hit.Distance;
							normal = glm::normalize(hit.Normal);
						}
					}
				}
				auto& ground = chain.Ground;
				if (!ground.Placed)
				{
					ground.Offset = offset;
					ground.Normal = normal;
					// Update(0) runs before physics bodies exist: the first real frame snaps instead of easing in.
					ground.Placed = seconds > 0;
				}
				else
				{
					ground.Offset += (offset - ground.Offset) * blend;
					const glm::vec3 eased = glm::mix(ground.Normal, normal, blend);
					ground.Normal = glm::length(eased) > 1e-6f ? glm::normalize(eased) : normal;
				}
				grounded = true;
				pelvisOffset = std::min(pelvisOffset, ground.Offset * resolved.Weight);
				resolved.Settled = std::abs(ground.Offset) < 1e-5f && glm::dot(ground.Normal, up) > 0.99999f;
				resolved.Target = foot + up * ground.Offset;
				// Turns the animated foot from the entity's up axis to the ground normal; converted to model space below.
				resolved.Rotation = RotationBetween(up, ground.Normal) * GlobalRotation(world * globals[end]);
			}
			if (std::holds_alternative<std::monostate>(chain.Target) && !config.Ground)
			{
				// No target: the chain idles.
				Report(chain.Problem, {}, entity, prefix);
				continue;
			}
			if (resolved.Weight <= 0)
			{
				Report(chain.Problem, {}, entity, prefix);
				continue;
			}
			resolved.Target = glm::vec3(model * glm::vec4(resolved.Target, 1.0f));
			if (resolved.Rotation)
				resolved.Rotation = toModelRotation(*resolved.Rotation);
			chains.push_back(std::move(resolved));
		}

		std::set<uint32_t> touched;
		if (!component.Pelvis.empty())
		{
			const int32_t pelvis = FindNode(source, component.Pelvis);
			if (pelvis < 0)
				problems.push_back(fmt::format("Pelvis joint '{}' not found in '{}'", component.Pelvis, source.Key));
			else if (source.Nodes[pelvis].HasMatrix)
				problems.push_back(fmt::format("Pelvis joint '{}' has a fixed matrix and cannot move", component.Pelvis));
			else if (grounded && pelvisOffset < 0)
			{
				// Down along the entity's up axis, expressed in the pelvis's parent space.
				glm::vec3 offset = glm::mat3(model) * (up * pelvisOffset);
				if (const int32_t parent = source.Nodes[pelvis].Parent; parent >= 0)
					offset = glm::inverse(glm::mat3(globals[parent])) * offset;
				const glm::vec3 animated = pose.Nodes[pelvis].Translation;
				pose.Nodes[pelvis].Translation += offset;
				if (Math::IsFinite(offset) && ComposeAnimationPose(source, pose, globals))
					touched.insert(static_cast<uint32_t>(pelvis));
				else
				{
					problems.push_back(fmt::format("Pelvis joint '{}' cannot be lowered: its parent has no finite inverse", component.Pelvis));
					pose.Nodes[pelvis].Translation = animated;
					ComposeAnimationPose(source, pose, globals);
				}
			}
		}
		for (const ResolvedChain& chain : chains)
		{
			const IKChain& config = *chain.Config;
			const std::string prefix = fmt::format("chain '{}': ", config.Name);
			// Unless the pelvis or an earlier chain moved something, a settled foot stays as animated.
			if (chain.Settled && touched.empty())
			{
				Report(*chain.Problem, {}, entity, prefix);
				continue;
			}
			IKSolveResult result;
			if (config.Type == IKChainType::TwoBone)
			{
				const bool pole = glm::length(config.Pole) > 0;
				result = SolveTwoBoneIK(source, pose, globals, { .Upper = chain.Joints[0], .Middle = chain.Joints[1], .End = chain.Joints[2], .Target = chain.Target, .Pole = pole ? std::optional<glm::vec3>(config.Pole) : std::nullopt, .EndRotation = chain.Rotation, .Weight = chain.Weight });
			}
			else
			{
				const float maxAngle = glm::radians(ClampSetting(config.MaxAngle, 0, 180, fmt::format("chain '{}' MaxAngle", config.Name), problems));
				result = SolveLookAtIK(source, pose, globals, { .Joints = chain.Joints, .Axis = config.Axis, .Target = chain.Target, .MaxAngle = maxAngle, .Weight = chain.Weight });
			}
			if (result == IKSolveResult::Solved)
				touched.insert(chain.Joints.begin(), chain.Joints.end());
			Report(*chain.Problem, DescribeIKResult(result), entity, prefix);
		}
		if (touched.empty())
			return finish(false);
		if (!std::ranges::all_of(globals, [](const glm::mat4& global) { return Math::IsFinite(global); }) || !SkinPalettesFinite(source, globals))
		{
			problems.emplace_back("the solved pose is not finite");
			return finish(false);
		}
		playback.IKOverlay.clear();
		for (uint32_t node : touched)
			playback.IKOverlay.emplace_back(node, pose.Nodes[node]);
		playback.OverlayDropped = false;
		playback.Globals = globals;
		finish(true);
	}

	AnimationSystem::IKChainState* AnimationSystem::FindChain(Entity entity, const std::string& chain)
	{
		const auto* component = entity.TryGetComponent<InverseKinematicsComponent>();
		if (!component)
		{
			ReportCallProblem(m_IKCallProblems, entity, "InverseKinematics", chain, "the entity has no InverseKinematics component");
			return nullptr;
		}
		IKState& state = SyncIK(entity, *component);
		if (auto found = std::ranges::find(state.Chains, chain, &IKChainState::Name); found != state.Chains.end())
			return &*found;
		ReportCallProblem(m_IKCallProblems, entity, "InverseKinematics", chain, fmt::format("no chain named '{}'", chain));
		return nullptr;
	}

	bool AnimationSystem::SetIKTarget(Entity entity, const std::string& chain, const glm::vec3& position, const std::optional<glm::quat>& rotation)
	{
		IKChainState* state = FindChain(entity, chain);
		if (!state)
			return false;
		const auto& config = *std::ranges::find(entity.GetComponent<InverseKinematicsComponent>().Chains, chain, &IKChain::Name);
		std::string problem;
		if (!Math::IsFinite(position) || (rotation && (!Math::IsFinite(*rotation) || glm::length(*rotation) < 1e-6f)))
			problem = fmt::format("chain '{}' target must be a finite position and a finite, nonzero rotation", chain);
		else if (rotation && config.Type == IKChainType::LookAt)
			problem = fmt::format("chain '{}' is a LookAt chain, which takes no rotation", chain);
		ReportCallProblem(m_IKCallProblems, entity, "InverseKinematics", chain, problem);
		if (!problem.empty())
			return false;
		state->Target = IKChainState::PositionTarget{ position, rotation ? std::optional<glm::quat>(glm::normalize(*rotation)) : std::nullopt };
		return true;
	}

	bool AnimationSystem::SetIKTarget(Entity entity, const std::string& chain, Entity target, bool matchRotation)
	{
		IKChainState* state = FindChain(entity, chain);
		if (!state)
			return false;
		const auto& config = *std::ranges::find(entity.GetComponent<InverseKinematicsComponent>().Chains, chain, &IKChain::Name);
		std::string problem;
		if (!target)
			problem = fmt::format("chain '{}' target entity does not exist", chain);
		else if (matchRotation && config.Type == IKChainType::LookAt)
			problem = fmt::format("chain '{}' is a LookAt chain, which takes no rotation", chain);
		ReportCallProblem(m_IKCallProblems, entity, "InverseKinematics", chain, problem);
		if (!problem.empty())
			return false;
		state->Target = IKChainState::EntityTarget{ target.GetUUID(), matchRotation };
		return true;
	}

	bool AnimationSystem::ClearIKTarget(Entity entity, const std::string& chain)
	{
		IKChainState* state = FindChain(entity, chain);
		if (!state)
			return false;
		ReportCallProblem(m_IKCallProblems, entity, "InverseKinematics", chain, {});
		state->Target = std::monostate{};
		// Ground placement takes over again from where the ground is now.
		state->Ground.Placed = false;
		return true;
	}

	bool AnimationSystem::SetIKWeight(Entity entity, const std::string& chain, float weight, float seconds)
	{
		IKChainState* state = FindChain(entity, chain);
		if (!state)
			return false;
		std::string problem;
		if (!std::isfinite(weight) || weight < 0 || weight > 1 || !std::isfinite(seconds) || seconds < 0)
			problem = fmt::format("chain '{}' weight must be between 0 and 1 and the fade time finite and nonnegative, got {} and {}", chain, weight, seconds);
		ReportCallProblem(m_IKCallProblems, entity, "InverseKinematics", chain, problem);
		if (!problem.empty())
			return false;
		state->Weight.FadeTo(weight, seconds);
		return true;
	}

	std::optional<float> AnimationSystem::GetIKWeight(Entity entity, const std::string& chain)
	{
		IKChainState* state = FindChain(entity, chain);
		if (!state)
			return std::nullopt;
		ReportCallProblem(m_IKCallProblems, entity, "InverseKinematics", chain, {});
		return state->Weight.Get();
	}

}
