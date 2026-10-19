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

// The inverse kinematics pass of AnimationSystem. It runs in LateUpdate: after physics and OnLateUpdate in a scene, at
// the end of Update for standalone callers.

namespace Basalt {

	namespace {
		constexpr const char* Owner = "InverseKinematics";

		// A chain resolved against the model for one solve.
		struct ResolvedChain
		{
			const IKChain* Config = nullptr;
			// The chain state's last problem, and this frame's problems so far: reported once the chain is solved.
			std::string* Problem = nullptr;
			std::vector<std::string> Problems;
			float Weight = 0;
			// TwoBone: upper, middle, end. LookAt: the aiming joint, then its ancestors.
			std::vector<uint32_t> Joints;
			// World space while resolving; model space once in the solve list. nullopt: nothing to reach for.
			std::optional<glm::vec3> Target;
			std::optional<glm::quat> Rotation;
			// A ground chain whose foot already stands on flat ground at its animated height. Solving it anyway would
			// still bend a straight leg by the solver's reach margin.
			bool Settled = false;
		};

		// Problems with a chain's settings that C++ code wrote past Validate.
		void CheckChainSettings(const IKChain& chain, std::vector<std::string>& problems)
		{
			if (static_cast<size_t>(chain.Type) >= IKChainTypeCount)
				problems.push_back(fmt::format("Type {} is not a chain type; ignoring the chain", static_cast<int>(chain.Type)));
			if (chain.Type == IKChainType::LookAt && (chain.ChainLength < 1 || chain.ChainLength > IKChain::MaxChainLength))
				problems.push_back(fmt::format("ChainLength must be between 1 and {}; clamping", IKChain::MaxChainLength));
			if (chain.Type == IKChainType::LookAt && chain.Ground)
				problems.emplace_back("Ground only applies to TwoBone chains; ignoring it");
			if (!Math::IsFinite(chain.Pole))
				problems.emplace_back("Pole is not finite; keeping the animated bend");
			if (!Math::IsFinite(chain.Axis) || glm::length(chain.Axis) <= 0)
				problems.emplace_back("Axis must be finite and nonzero");
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
		state.Chains = MatchByName(std::move(state.Chains), component.Chains, [](const IKChain& config) { return IKChainState{ .Name = config.Name, .Weight = FadingWeight(config.Weight) }; });
		for (size_t i = 0; i < component.Chains.size(); i++)
			state.Chains[i].Weight.Configure(component.Chains[i].Weight);
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
			// Ensure already warns about an empty Mesh.
			else if (const auto* animator = entity.TryGetComponent<AnimatorComponent>(); !animator || !animator->Mesh.empty())
				ReportProblem(state.Problem, "not solved: " + DescribeUnplayable(entity), entity, Owner);
		}
	}

	void AnimationSystem::SolveEntityIK(Entity entity, Playback& playback, const InverseKinematicsComponent& component, IKState& state, float seconds)
	{
		BS_CORE_ASSERT(state.Chains.size() == component.Chains.size(), "SyncIK builds one state per chain");
		const MeshSource& source = *playback.Source;
		std::vector<std::string> problems;
		const auto finish = [&](bool solved) {
			// Without a solution the pose returns to the animation alone.
			if (!solved)
				DropOverlay(playback, &Playback::IKOverlay);
			ReportProblem(state.Problem, JoinProblems(problems), entity, Owner);
		};

		// Settings first and every frame, so their problems do not come and go with targets and weights.
		const InverseKinematicsComponent defaults;
		const float rayHeight = ClampSetting(component.RayHeight, 0, 100, defaults.RayHeight, "RayHeight", problems);
		const float rayDepth = ClampSetting(component.RayDepth, 0, 100, defaults.RayDepth, "RayDepth", problems);
		const float smoothing = ClampSetting(component.Smoothing, 0, 100, defaults.Smoothing, "Smoothing", problems);
		const bool groundChains = std::ranges::any_of(component.Chains, [](const IKChain& chain) { return chain.Ground && chain.Type == IKChainType::TwoBone; });
		if (groundChains && rayHeight + rayDepth <= 0)
			problems.emplace_back("RayHeight + RayDepth must be positive for Ground chains; feet keep their animation");
		for (const IKChain& config : component.Chains)
		{
			ClampSetting(config.Weight, 0, 1, IKChain{}.Weight, fmt::format("chain '{}' Weight", config.Name), problems);
			if (config.Type == IKChainType::LookAt)
				ClampSetting(config.MaxAngle, 0, 180, IKChain{}.MaxAngle, fmt::format("chain '{}' MaxAngle", config.Name), problems);
		}

		// Always from the layered pose without IK, so held poses never accumulate last frame's solution.
		AnimationLocalPose pose = playback.Pose;
		std::vector<glm::mat4>& globals = m_OverlayGlobals;
		if (!ComposeAnimationPose(source, pose, globals))
		{
			problems.emplace_back("the animated pose is not finite");
			return finish(false);
		}
		const glm::mat4 world = m_Scene.GetWorldTransform(entity);
		// A zero scale (e.g. a spawn effect growing the entity) has nothing to bend; that is no problem.
		if (glm::length(glm::vec3(world[0])) == 0 || glm::length(glm::vec3(world[1])) == 0 || glm::length(glm::vec3(world[2])) == 0)
			return finish(false);
		const glm::mat4 model = glm::inverse(world);
		if (!Math::IsFinite(model))
		{
			problems.emplace_back("the entity's transform is not invertible (degenerate or not finite)");
			return finish(false);
		}
		// World rotations into the model's space (GlobalRotation keeps a mirrored transform's rotation proper).
		const auto toModelRotation = [&](const glm::quat& rotation) { return GlobalRotation(model * glm::mat4_cast(rotation)); };
		const glm::vec3 up = glm::normalize(glm::vec3(world[1]));
		const glm::vec3 origin = glm::vec3(world[3]);

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
			std::vector<std::string> chainProblems;
			CheckChainSettings(config, chainProblems);
			ResolvedChain resolved{ .Config = &config, .Problem = &chain.Problem, .Weight = chain.Weight.Get() };
			if (static_cast<size_t>(config.Type) >= IKChainTypeCount)
			{
				ReportProblem(chain.Problem, JoinProblems(chainProblems), entity, Owner, &config.Name);
				continue;
			}
			if (std::string problem = ResolveIKChain(source, config, resolved.Joints); !problem.empty())
			{
				chainProblems.push_back(std::move(problem));
				ReportProblem(chain.Problem, JoinProblems(chainProblems), entity, Owner, &config.Name);
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
					chainProblems.emplace_back("its target entity no longer exists");
					ReportProblem(chain.Problem, JoinProblems(chainProblems), entity, Owner, &config.Name);
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
						// `body` is the entity or an ancestor of it, so the entity's descendants are below it.
						const Entity hitEntity = m_Scene.GetEntityByUUID(hit.EntityID);
						if (hitEntity && (hitEntity == entity || m_Scene.IsDescendantOf(hitEntity, body)))
							continue;
						// Surfaces facing away from up (undersides, and hits from inside a body, which have no normal)
						// are not ground.
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
				// Turns the animated foot from the entity's up axis to the ground normal.
				resolved.Rotation = RotationBetween(up, ground.Normal) * GlobalRotation(world * globals[end]);
			}
			if (resolved.Rotation && config.Type == IKChainType::LookAt)
			{
				// The chain's Type changed after a rotation target was set.
				chainProblems.emplace_back("a LookAt chain takes no rotation; ignoring the target's");
				resolved.Rotation.reset();
			}
			// No target (the chain idles) or no weight.
			if (!resolved.Target || resolved.Weight <= 0)
			{
				ReportProblem(chain.Problem, JoinProblems(chainProblems), entity, Owner, &config.Name);
				continue;
			}
			resolved.Problems = std::move(chainProblems);
			resolved.Target = glm::vec3(model * glm::vec4(*resolved.Target, 1.0f));
			if (resolved.Rotation)
				resolved.Rotation = toModelRotation(*resolved.Rotation);
			chains.push_back(std::move(resolved));
		}

		std::vector<uint32_t>& touched = m_TouchedNodes;
		touched.clear();
		std::string pelvisProblem;
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
					touched.push_back(static_cast<uint32_t>(pelvis));
				else
				{
					pelvisProblem = fmt::format("Pelvis joint '{}' cannot be lowered: the lowered pose is not finite", component.Pelvis);
					pose.Nodes[pelvis].Translation = animated;
					// The pose composed before the pelvis moved, so this cannot fail.
					ComposeAnimationPose(source, pose, globals);
				}
			}
		}
		// Only while lowering, so it has its own slot: the entity's other problems are not repeated when it comes and goes.
		ReportProblem(state.PelvisProblem, std::move(pelvisProblem), entity, Owner);
		for (ResolvedChain& chain : chains)
		{
			const IKChain& config = *chain.Config;
			const auto report = [&] { ReportProblem(*chain.Problem, JoinProblems(chain.Problems), entity, Owner, &config.Name); };
			// A settled foot stays as animated unless an earlier chain or the pelvis moved one of its leg's joints or a node
			// above them.
			const uint32_t upper = chain.Joints.front();
			const auto movesLeg = [&](uint32_t node) { return std::ranges::find(chain.Joints, node) != chain.Joints.end() || IsAncestor(source, node, upper); };
			if (chain.Settled && std::ranges::none_of(touched, movesLeg))
			{
				report();
				continue;
			}
			IKSolveResult result;
			if (config.Type == IKChainType::TwoBone)
			{
				// A zero Pole keeps the animated bend; a non-finite one was reported and does too.
				const std::optional<glm::vec3> pole = Math::IsFinite(config.Pole) ? std::optional<glm::vec3>(config.Pole) : std::nullopt;
				result = SolveTwoBoneIK(source, pose, globals, { .Upper = chain.Joints[0], .Middle = chain.Joints[1], .End = chain.Joints[2], .Target = *chain.Target, .Pole = pole, .EndRotation = chain.Rotation, .Weight = chain.Weight });
			}
			else
			{
				const float maxAngle = std::isfinite(config.MaxAngle) ? std::clamp(config.MaxAngle, 0.0f, 180.0f) : 0.0f;
				result = SolveLookAtIK(source, pose, globals, { .Joints = chain.Joints, .Axis = config.Axis, .Target = *chain.Target, .MaxAngleRadians = glm::radians(maxAngle), .Weight = chain.Weight });
			}
			if (result == IKSolveResult::Solved)
				touched.insert(touched.end(), chain.Joints.begin(), chain.Joints.end());
			else if (result != IKSolveResult::Unchanged)
				chain.Problems.emplace_back(DescribeIKResult(result));
			report();
		}
		if (touched.empty())
			return finish(false);
		// The solvers leave globals composed (finite); skins add their inverse binds.
		if (!SkinPalettesFinite(source, globals))
		{
			problems.emplace_back("the solved pose is not finite");
			return finish(false);
		}
		StoreOverlay(playback, &Playback::IKOverlay, touched, pose, globals);
		finish(true);
	}

	AnimationSystem::ChainLookup AnimationSystem::FindChain(Entity entity, const std::string& chain, const char* operation)
	{
		const auto* component = entity.TryGetComponent<InverseKinematicsComponent>();
		if (!component)
		{
			ReportIKCall(entity, operation, {}, "the entity has no InverseKinematics component");
			return {};
		}
		const auto config = std::ranges::find(component->Chains, chain, &IKChain::Name);
		if (config == component->Chains.end())
		{
			// Unknown names share one slot per operation (see ReportCallProblem).
			ReportIKCall(entity, operation, {}, fmt::format("no chain named '{}'", chain));
			return {};
		}
		ReportIKCall(entity, operation, {}, {});
		IKState& state = SyncIK(entity, *component);
		return { &state.Chains[static_cast<size_t>(config - component->Chains.begin())], &*config };
	}

	bool AnimationSystem::SetIKTarget(Entity entity, const std::string& chain, const glm::vec3& position, const std::optional<glm::quat>& rotation)
	{
		constexpr const char* operation = "SetIKTarget";
		const auto [state, config] = FindChain(entity, chain, operation);
		if (!state)
			return false;
		std::string problem;
		if (!Math::IsFinite(position))
			problem = fmt::format("chain '{}' target position must be finite", chain);
		else if (rotation && (!Math::IsFinite(*rotation) || glm::length(*rotation) < 1e-6f))
			problem = fmt::format("chain '{}' target rotation must be finite and nonzero", chain);
		else if (rotation && config->Type == IKChainType::LookAt)
			problem = fmt::format("chain '{}' is a LookAt chain, which takes no rotation", chain);
		ReportIKCall(entity, operation, chain, problem);
		if (!problem.empty())
			return false;
		state->Target = IKChainState::PositionTarget{ position, rotation ? std::optional<glm::quat>(glm::normalize(*rotation)) : std::nullopt };
		return true;
	}

	bool AnimationSystem::SetIKTarget(Entity entity, const std::string& chain, Entity target, bool matchRotation)
	{
		constexpr const char* operation = "SetIKTarget";
		const auto [state, config] = FindChain(entity, chain, operation);
		if (!state)
			return false;
		std::string problem;
		if (!target)
			problem = fmt::format("chain '{}' target entity does not exist", chain);
		else if (matchRotation && config->Type == IKChainType::LookAt)
			problem = fmt::format("chain '{}' is a LookAt chain, which takes no rotation", chain);
		ReportIKCall(entity, operation, chain, problem);
		if (!problem.empty())
			return false;
		state->Target = IKChainState::EntityTarget{ target.GetUUID(), matchRotation };
		return true;
	}

	bool AnimationSystem::ClearIKTarget(Entity entity, const std::string& chain)
	{
		const auto [state, config] = FindChain(entity, chain, "ClearIKTarget");
		if (!state)
			return false;
		state->Target = std::monostate{};
		// Ground placement takes over again from where the ground is now.
		state->Ground.Placed = false;
		return true;
	}

	bool AnimationSystem::SetIKWeight(Entity entity, const std::string& chain, float weight, float seconds)
	{
		constexpr const char* operation = "SetIKWeight";
		const auto [state, config] = FindChain(entity, chain, operation);
		if (!state)
			return false;
		if (!IsValidFade(weight, seconds))
		{
			// The values stay out of the compared problem, so a script computing a different bad value every frame logs
			// once.
			ReportIKCall(entity, operation, chain, fmt::format("chain '{}': the weight must be between 0 and 1 and the fade time finite and nonnegative", chain), fmt::format(" (got {} and {})", weight, seconds));
			return false;
		}
		ReportIKCall(entity, operation, chain, {});
		state->Weight.FadeTo(weight, seconds);
		return true;
	}

	std::optional<float> AnimationSystem::GetIKWeight(Entity entity, const std::string& chain)
	{
		const auto [state, config] = FindChain(entity, chain, "GetIKWeight");
		if (!state)
			return std::nullopt;
		return state->Weight.Get();
	}

}
