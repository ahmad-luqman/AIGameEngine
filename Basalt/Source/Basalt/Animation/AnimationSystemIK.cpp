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
#include <set>

// The inverse kinematics pass of AnimationSystem: it runs in LateUpdate, after physics and OnLateUpdate.

namespace Basalt {

	namespace {
		int32_t FindJoint(const MeshSource& source, const std::string& name)
		{
			const auto node = std::ranges::find(source.Nodes, name, &MeshNode::Name);
			return node == source.Nodes.end() ? -1 : static_cast<int32_t>(node - source.Nodes.begin());
		}

		bool IsFiniteVector(const glm::vec3& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}

		bool IsFiniteRotation(const glm::quat& q)
		{
			return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w) && glm::length(q) > 1e-6f;
		}

		void FadeWeight(float& weight, float target, float& rate, float seconds)
		{
			if (rate <= 0 || seconds <= 0)
				return;
			const float step = seconds * rate;
			weight = weight < target ? std::min(weight + step, target) : std::max(weight - step, target);
			if (weight == target)
				rate = 0;
		}

		// A chain resolved against the model for one solve.
		struct ResolvedChain
		{
			const IKChain* Config = nullptr;
			float Weight = 0;
			// TwoBone: upper, middle, end. LookAt: the aiming joint, then its ancestors.
			std::vector<uint32_t> Joints;
			// Model space.
			glm::vec3 Target = glm::vec3(0);
			std::optional<glm::quat> Rotation;
			// A ground chain whose foot already stands on flat ground at its animated height.
			bool Settled = false;
		};
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
			IKChainState chain;
			if (auto found = std::ranges::find(state.Chains, config.Name, &IKChainState::Name); found != state.Chains.end())
				chain = std::move(*found);
			else
			{
				chain.Name = config.Name;
				chain.ConfiguredWeight = chain.Weight = chain.TargetWeight = config.Weight;
			}
			if (config.Weight != chain.ConfiguredWeight)
			{
				chain.ConfiguredWeight = chain.Weight = chain.TargetWeight = config.Weight;
				chain.FadeRate = 0;
			}
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
				FadeWeight(chain.Weight, chain.TargetWeight, chain.FadeRate, seconds);
			// Authoring warnings report an InverseKinematics without a playable Animator.
			if (auto* playback = Ensure(entity))
				SolveEntityIK(entity, *playback, component, state, seconds);
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
			std::string problem = fmt::format("{}", fmt::join(problems, "; "));
			if (!problem.empty() && problem != state.Problem)
				BS_CORE_WARN("InverseKinematics '{}': {}", entity.GetName(), problem);
			state.Problem = std::move(problem);
		};

		// Always from the animated pose without IK, so held poses never accumulate last frame's solution.
		AnimationLocalPose pose = playback.Pose;
		std::vector<glm::mat4>& globals = m_IKGlobals;
		if (!ComposeAnimationPose(source, pose, globals))
			return finish(false);
		const glm::mat4 world = m_Scene.GetWorldTransform(entity);
		const glm::mat4 model = glm::inverse(world);
		if (!Math::IsFinite(model) || std::abs(glm::determinant(glm::mat3(world))) < 1e-12f)
		{
			problems.emplace_back("the entity's transform has no inverse (zero scale)");
			return finish(false);
		}
		const glm::quat modelRotation = GlobalRotation(model);
		glm::vec3 up = glm::vec3(world[1]);
		up = glm::length(up) > 0 ? glm::normalize(up) : glm::vec3(0, 1, 0);
		const glm::vec3 origin = glm::vec3(world[3]);

		PhysicsWorld* physics = m_Scene.GetPhysicsWorld();
		// Feet follow the ground only while the character stands on it, so a jump keeps its animated legs.
		bool airborne = false;
		if (physics)
			for (Entity holder = entity; holder; holder = holder.GetParent())
				if (holder.HasComponent<CharacterControllerComponent>() && physics->HasCharacter(holder))
				{
					airborne = !physics->IsCharacterGrounded(holder);
					break;
				}
		const float blend = component.Smoothing > 0 ? 1.0f - std::exp(-seconds / component.Smoothing) : 1.0f;

		std::vector<ResolvedChain> chains;
		float pelvisOffset = 0;
		bool grounded = false;
		for (const IKChain& config : component.Chains)
		{
			auto found = std::ranges::find(state.Chains, config.Name, &IKChainState::Name);
			IKChainState& chain = *found;
			const auto fail = [&](const std::string& problem) { problems.push_back(fmt::format("chain '{}': {}", config.Name, problem)); };
			if (config.Joint.empty())
			{
				fail("Joint is empty");
				continue;
			}
			const int32_t end = FindJoint(source, config.Joint);
			if (end < 0)
			{
				fail(fmt::format("joint '{}' not found in '{}'", config.Joint, source.Key));
				continue;
			}
			ResolvedChain resolved{ .Config = &config, .Weight = chain.Weight };
			const size_t length = config.Type == IKChainType::TwoBone ? 3 : std::clamp<uint32_t>(config.ChainLength, 1, IKChain::MaxChainLength);
			for (int32_t node = end; node >= 0 && resolved.Joints.size() < length; node = source.Nodes[node].Parent)
				resolved.Joints.push_back(static_cast<uint32_t>(node));
			if (resolved.Joints.size() < length)
			{
				fail(config.Type == IKChainType::TwoBone ? fmt::format("joint '{}' needs a parent and a grandparent", config.Joint) : fmt::format("joint '{}' has fewer than {} joints above it", config.Joint, length - 1));
				continue;
			}
			if (std::ranges::any_of(resolved.Joints, [&](uint32_t joint) { return source.Nodes[joint].HasMatrix; }))
			{
				fail("a joint has a fixed matrix and cannot be turned");
				continue;
			}
			if (config.Type == IKChainType::TwoBone)
				std::ranges::reverse(resolved.Joints);

			switch (chain.Kind)
			{
				case IKChainState::TargetKind::Position:
					resolved.Target = chain.Position;
					resolved.Rotation = chain.Rotation;
					break;
				case IKChainState::TargetKind::Entity:
				{
					const Entity target = m_Scene.GetEntityByUUID(chain.TargetEntity);
					if (!target)
					{
						fail("its target entity no longer exists");
						continue;
					}
					const glm::mat4 transform = m_Scene.GetWorldTransform(target);
					resolved.Target = glm::vec3(transform[3]);
					if (chain.MatchRotation)
						resolved.Rotation = GlobalRotation(transform);
					break;
				}
				case IKChainState::TargetKind::None:
				{
					if (!config.Ground || config.Type != IKChainType::TwoBone)
						continue;
					// The ground below the animated foot, relative to the plane through the entity's origin: the
					// animation is authored on that plane, so lifts and steps keep their height above the ground.
					const glm::vec3 foot = glm::vec3(world * globals[end][3]);
					const glm::vec3 base = foot - up * glm::dot(foot - origin, up);
					float offset = 0;
					glm::vec3 normal = up;
					if (physics && !airborne)
					{
						const PhysicsQueryFilter filter{ .IgnoreEntity = entity.GetUUID() };
						float nearest = std::numeric_limits<float>::max();
						for (const RaycastHit& hit : physics->RaycastAll(base + up * component.RayHeight, -up, component.RayHeight + component.RayDepth, filter))
						{
							const Entity hitEntity = m_Scene.GetEntityByUUID(hit.EntityID);
							// The character's own bodies (a parent's controller, a child's hitboxes) are not ground.
							if (hitEntity && (m_Scene.IsDescendantOf(hitEntity, entity) || m_Scene.IsDescendantOf(entity, hitEntity)))
								continue;
							if (hit.Distance < nearest && glm::dot(hit.Normal, up) > 0)
							{
								nearest = hit.Distance;
								offset = component.RayHeight - hit.Distance;
								normal = glm::normalize(hit.Normal);
							}
						}
					}
					if (!chain.Placed)
					{
						chain.GroundOffset = offset;
						chain.GroundNormal = normal;
						// Update(0) runs before physics bodies exist: the first real frame snaps instead of easing in.
						chain.Placed = seconds > 0;
					}
					else
					{
						chain.GroundOffset += (offset - chain.GroundOffset) * blend;
						const glm::vec3 eased = glm::mix(chain.GroundNormal, normal, blend);
						chain.GroundNormal = glm::length(eased) > 1e-6f ? glm::normalize(eased) : normal;
					}
					grounded = true;
					pelvisOffset = std::min(pelvisOffset, chain.GroundOffset * chain.Weight);
					resolved.Settled = std::abs(chain.GroundOffset) < 1e-5f && glm::dot(chain.GroundNormal, up) > 0.99999f;
					resolved.Target = foot + up * chain.GroundOffset;
					resolved.Rotation = RotationBetween(up, chain.GroundNormal) * GlobalRotation(world * globals[end]);
					break;
				}
			}
			if (resolved.Weight <= 0)
				continue;
			resolved.Target = glm::vec3(model * glm::vec4(resolved.Target, 1.0f));
			if (resolved.Rotation)
				resolved.Rotation = glm::normalize(modelRotation * *resolved.Rotation);
			chains.push_back(std::move(resolved));
		}

		std::set<uint32_t> touched;
		if (grounded && pelvisOffset < 0 && !component.Pelvis.empty())
		{
			const int32_t pelvis = FindJoint(source, component.Pelvis);
			if (pelvis < 0)
				problems.push_back(fmt::format("Pelvis joint '{}' not found in '{}'", component.Pelvis, source.Key));
			else if (source.Nodes[pelvis].HasMatrix)
				problems.push_back(fmt::format("Pelvis joint '{}' has a fixed matrix and cannot move", component.Pelvis));
			else
			{
				// Down along the entity's up axis, expressed in the pelvis's parent space.
				glm::vec3 offset = glm::mat3(model) * (up * pelvisOffset);
				if (const int32_t parent = source.Nodes[pelvis].Parent; parent >= 0)
					offset = glm::inverse(glm::mat3(globals[parent])) * offset;
				if (IsFiniteVector(offset))
				{
					pose.Nodes[pelvis].Translation += offset;
					touched.insert(static_cast<uint32_t>(pelvis));
					ComposeAnimationPose(source, pose, globals);
				}
			}
		}
		for (const ResolvedChain& chain : chains)
		{
			const IKChain& config = *chain.Config;
			// A foot already on its animated spot needs no solve unless the pelvis moved.
			if (chain.Settled && touched.empty())
				continue;
			bool solved = false;
			if (config.Type == IKChainType::TwoBone)
				solved = SolveTwoBoneIK(source, pose, globals, chain.Joints[0], chain.Joints[1], chain.Joints[2], chain.Target, config.Pole, chain.Rotation, chain.Weight);
			else
				solved = SolveLookAtIK(source, pose, globals, chain.Joints, config.Axis, chain.Target, glm::radians(config.MaxAngle), chain.Weight);
			if (!solved)
			{
				problems.push_back(fmt::format("chain '{}' has zero-length bones or no finite solution", config.Name));
				continue;
			}
			touched.insert(chain.Joints.begin(), chain.Joints.end());
		}
		if (touched.empty())
			return finish(false);
		bool valid = true;
		for (const auto& skin : source.Skins)
			valid = valid && !BuildSkinPalette(skin, globals).empty();
		if (!valid)
		{
			problems.emplace_back("the solved pose is not finite");
			return finish(false);
		}
		playback.IKOverlay.clear();
		for (uint32_t node : touched)
			playback.IKOverlay.emplace_back(node, pose.Nodes[node]);
		playback.Globals = globals;
		finish(true);
	}

	AnimationSystem::IKChainState* AnimationSystem::FindChain(Entity entity, const std::string& chain)
	{
		const auto* component = entity.TryGetComponent<InverseKinematicsComponent>();
		if (!component)
		{
			ReportIKCallProblem(entity, "the entity has no InverseKinematics component");
			return nullptr;
		}
		IKState& state = SyncIK(entity, *component);
		if (auto found = std::ranges::find(state.Chains, chain, &IKChainState::Name); found != state.Chains.end())
			return &*found;
		ReportIKCallProblem(entity, fmt::format("no chain named '{}'", chain));
		return nullptr;
	}

	void AnimationSystem::ReportIKCallProblem(Entity entity, const std::string& problem)
	{
		if (problem.empty())
		{
			m_IKCallProblems.erase(entity.GetUUID());
			return;
		}
		std::string& reported = m_IKCallProblems[entity.GetUUID()];
		if (reported != problem)
			BS_CORE_ERROR("InverseKinematics '{}': {}", entity.GetName(), problem);
		reported = problem;
	}

	bool AnimationSystem::SetIKTarget(Entity entity, const std::string& chain, const glm::vec3& position, const std::optional<glm::quat>& rotation)
	{
		if (!IsFiniteVector(position) || (rotation && !IsFiniteRotation(*rotation)))
		{
			BS_CORE_ERROR("InverseKinematics '{}': chain '{}' target must be finite", entity.GetName(), chain);
			return false;
		}
		IKChainState* state = FindChain(entity, chain);
		if (!state)
			return false;
		ReportIKCallProblem(entity, {});
		state->Kind = IKChainState::TargetKind::Position;
		state->Position = position;
		state->Rotation = rotation ? std::optional<glm::quat>(glm::normalize(*rotation)) : std::nullopt;
		return true;
	}

	bool AnimationSystem::SetIKTarget(Entity entity, const std::string& chain, Entity target, bool matchRotation)
	{
		if (!target)
		{
			BS_CORE_ERROR("InverseKinematics '{}': chain '{}' target entity does not exist", entity.GetName(), chain);
			return false;
		}
		IKChainState* state = FindChain(entity, chain);
		if (!state)
			return false;
		ReportIKCallProblem(entity, {});
		state->Kind = IKChainState::TargetKind::Entity;
		state->TargetEntity = target.GetUUID();
		state->MatchRotation = matchRotation;
		return true;
	}

	bool AnimationSystem::ClearIKTarget(Entity entity, const std::string& chain)
	{
		IKChainState* state = FindChain(entity, chain);
		if (!state)
			return false;
		ReportIKCallProblem(entity, {});
		state->Kind = IKChainState::TargetKind::None;
		state->Rotation.reset();
		state->TargetEntity = 0;
		// Ground placement takes over again from where the ground is now.
		state->Placed = false;
		return true;
	}

	bool AnimationSystem::SetIKWeight(Entity entity, const std::string& chain, float weight, float seconds)
	{
		if (!std::isfinite(weight) || weight < 0 || weight > 1 || !std::isfinite(seconds) || seconds < 0)
		{
			BS_CORE_ERROR("InverseKinematics '{}': chain '{}' weight must be between 0 and 1 and the fade time finite and nonnegative, got {} and {}", entity.GetName(), chain, weight, seconds);
			return false;
		}
		IKChainState* state = FindChain(entity, chain);
		if (!state)
			return false;
		ReportIKCallProblem(entity, {});
		state->TargetWeight = weight;
		if (seconds > 0 && weight != state->Weight)
			state->FadeRate = std::abs(weight - state->Weight) / seconds;
		else
		{
			state->Weight = weight;
			state->FadeRate = 0;
		}
		return true;
	}

	std::optional<float> AnimationSystem::GetIKWeight(Entity entity, const std::string& chain)
	{
		IKChainState* state = FindChain(entity, chain);
		if (!state)
			return std::nullopt;
		ReportIKCallProblem(entity, {});
		return state->Weight;
	}

}
