#include "Basalt/Physics/PhysicsWorld.h"

#include "Basalt/Animation/AnimationSystem.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Physics/PhysicsWorldImpl.h"
#include "Basalt/Physics/Ragdoll.h"

#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace Basalt {

	using namespace PhysicsInternal;

	namespace {
		bool UniformTransform(const glm::mat4& matrix, glm::vec3& position, glm::quat& rotation, glm::vec3& scale)
		{
			return Math::IsFinite(matrix) && Math::DecomposeAffineTransform(matrix, position, rotation, scale) && scale.x > 1e-5f &&
				   std::abs(scale.y - scale.x) <= 1e-4f * scale.x && std::abs(scale.z - scale.x) <= 1e-4f * scale.x;
		}
	}

	bool PhysicsWorld::Impl::BuildRagdoll(Entity entity, RagdollRecord& record, std::string& error)
	{
		const auto& component = entity.GetComponent<RagdollComponent>();
		error = ValidateRagdoll(component);
		if (!error.empty())
			return false;
		auto* animations = ScenePtr->GetAnimationSystem();
		const auto view = animations ? animations->GetPoseView(entity) : std::nullopt;
		if (!view)
		{
			error = "requires a loadable Animator with a skeleton";
			return false;
		}
		const auto& source = *view->Source;
		const int32_t root = component.RootJoint.empty() ? -1 : FindNode(source, component.RootJoint);
		if (!component.RootJoint.empty() && root < 0)
		{
			error = "RootJoint '" + component.RootJoint + "' does not exist";
			return false;
		}
		std::set<uint32_t> selected;
		for (const auto& skin : source.Skins)
			for (const auto& joint : skin.Joints)
				if (root < 0 || joint.Node == static_cast<uint32_t>(root) || IsAncestor(source, static_cast<uint32_t>(root), joint.Node))
					selected.insert(joint.Node);
		if (selected.empty() || selected.size() > RagdollComponent::MaxBones || (root >= 0 && !selected.contains(static_cast<uint32_t>(root))))
		{
			error = "RootJoint must select 1..128 skin joints";
			return false;
		}
		std::map<uint32_t, RagdollBone> overrides;
		for (const auto& bone : component.Bones)
		{
			const int32_t node = FindNode(source, bone.Joint);
			if (node < 0 || !selected.contains(static_cast<uint32_t>(node)))
			{
				error = "Bones joint '" + bone.Joint + "' is not a selected skin joint";
				return false;
			}
			overrides.emplace(static_cast<uint32_t>(node), bone);
		}
		std::vector<glm::mat4> rest;
		if (!EvaluateAnimation(source, nullptr, 0, rest))
		{
			error = "non-finite rest pose";
			return false;
		}
		const glm::mat4 world = ScenePtr->GetWorldTransform(entity);
		glm::vec3 position, scale;
		glm::quat rotation;
		if (!UniformTransform(world, position, rotation, scale))
		{
			error = "requires positive uniform world scale without shear";
			return false;
		}
		JPH::Ref<JPH::RagdollSettings> settings = new JPH::RagdollSettings;
		settings->mSkeleton = new JPH::Skeleton;
		std::map<uint32_t, int> indices;
		std::vector<JPH::Mat44> initial, restMatrices;
		for (uint32_t node : source.NodeOrder)
		{
			if (!selected.contains(node))
				continue;
			// Bodies can skip non-skin spacer nodes, but the resulting skeleton must have one root.
			int32_t parent = source.Nodes[node].Parent;
			while (parent >= 0 && !indices.contains(static_cast<uint32_t>(parent)))
				parent = source.Nodes[parent].Parent;
			if (parent < 0 && !indices.empty())
			{
				error = "selected skin joints have multiple roots; set RootJoint to one subtree";
				return false;
			}
			if (source.Nodes[node].HasMatrix)
			{
				error = "joint '" + source.Nodes[node].Name + "' uses a matrix; TRS joints are required";
				return false;
			}
			const int parentIndex = parent < 0 ? -1 : indices.at(static_cast<uint32_t>(parent));
			indices.emplace(node, static_cast<int>(indices.size()));
			settings->mSkeleton->AddJoint(source.Nodes[node].Name, parentIndex);
			glm::vec3 restPosition, restScale;
			glm::quat restRotation;
			if (!UniformTransform(world * rest[node], restPosition, restRotation, restScale) || !UniformTransform(world * view->Globals[node], position, rotation, scale) || std::abs(scale.x - restScale.x) > 1e-4f * restScale.x)
			{
				error = "joint '" + source.Nodes[node].Name + "' requires constant positive uniform scale without shear";
				return false;
			}
			// Choose the first non-coincident skin descendant in topological order. Leaves continue the incoming bone.
			glm::vec3 direction(0, 1, 0);
			float length = 0.1f * restScale.x;
			bool hasLength = false;
			for (uint32_t child : source.NodeOrder)
				if (selected.contains(child) && IsAncestor(source, node, child))
				{
					const glm::vec3 delta = glm::vec3(world * rest[child] * glm::vec4(0, 0, 0, 1)) - restPosition;
					if (glm::length(delta) > 1e-5f)
					{
						length = glm::length(delta);
						direction = delta / length;
						hasLength = true;
						break;
					}
				}
			if (!hasLength && parent >= 0)
			{
				const glm::vec3 delta = restPosition - glm::vec3(world * rest[parent] * glm::vec4(0, 0, 0, 1));
				if (glm::length(delta) > 1e-5f)
				{
					length = glm::length(delta) * 0.5f;
					direction = glm::normalize(delta);
				}
			}
			const RagdollBone bone = overrides.contains(node) ? overrides.at(node) : RagdollBone{};
			if (bone.Length > 0)
				length = bone.Length * restScale.x;
			const float radius = bone.Radius > 0 ? bone.Radius * restScale.x : length * component.RadiusRatio;
			length = std::max(length, 2 * radius);
			if (!std::isfinite(length) || !std::isfinite(radius) || radius < 1e-5f || length > 10000)
			{
				error = "joint '" + source.Nodes[node].Name + "' has an unsupported generated capsule size";
				return false;
			}
			const glm::vec3 localDirection = glm::inverse(restRotation) * direction;
			JPH::RefConst<JPH::Shape> capsule;
			if (length > 2 * radius + 1e-5f)
				capsule = new JPH::CapsuleShape(0.5f * length - radius, radius);
			else
				capsule = new JPH::SphereShape(radius);
			JPH::RagdollSettings::Part part;
			part.SetShape(new JPH::RotatedTranslatedShape(ToJolt(localDirection * (0.5f * length)), JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), ToJolt(localDirection).Normalized()), capsule));
			part.mPosition = ToJolt(restPosition);
			part.mRotation = ToJolt(restRotation);
			part.mMotionType = JPH::EMotionType::Dynamic;
			const auto layer = Layers.Find(component.Layer);
			if (!layer)
			{
				error = "unknown Layer '" + component.Layer + "'";
				return false;
			}
			part.mObjectLayer = MakeObjectLayer(true, *layer);
			part.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
			part.mMassPropertiesOverride.mMass = bone.Mass > 0 ? bone.Mass : component.Mass / static_cast<float>(selected.size());
			part.mFriction = 0.5f;
			if (parentIndex >= 0)
			{
				JPH::Ref<JPH::SwingTwistConstraintSettings> constraint = new JPH::SwingTwistConstraintSettings;
				constraint->mPosition1 = constraint->mPosition2 = ToJolt(restPosition);
				constraint->mTwistAxis1 = constraint->mTwistAxis2 = ToJolt(direction);
				constraint->mPlaneAxis1 = constraint->mPlaneAxis2 = ToJolt(direction).GetNormalizedPerpendicular();
				constraint->mNormalHalfConeAngle = constraint->mPlaneHalfConeAngle = glm::radians(bone.SwingAngle);
				constraint->mTwistMinAngle = glm::radians(bone.TwistMin);
				constraint->mTwistMaxAngle = glm::radians(bone.TwistMax);
				part.mToParent = constraint;
			}
			settings->mParts.push_back(part);
			record.Nodes.push_back(node);
			record.Scales.push_back(restScale.x);
			record.Capsules.push_back({ localDirection * radius, localDirection * (length - radius), radius });
			initial.push_back(JPH::Mat44::sRotationTranslation(ToJolt(rotation), ToJolt(position)));
			restMatrices.push_back(JPH::Mat44::sRotationTranslation(ToJolt(restRotation), ToJolt(restPosition)));
		}
		settings->DisableParentChildCollisions(restMatrices.data());
		if (!settings->Stabilize())
		{
			error = "could not stabilize the ragdoll constraints";
			return false;
		}
		settings->CalculateConstraintPriorities();
		settings->CalculateBodyIndexToConstraintIndex();
		settings->CalculateConstraintIndexToBodyIdxPair();
		record.Doll = settings->CreateRagdoll(NextRagdollGroup++, static_cast<uint64_t>(entity.GetUUID()), System.get());
		if (!record.Doll)
		{
			error = "Jolt body capacity exhausted";
			return false;
		}
		record.World = world;
		record.Source = view->Source;
		record.Settings = component;
		record.InitialRoot = FromJolt(initial.front().GetTranslation());
		record.Doll->SetPose(JPH::RVec3::sZero(), initial.data());
		return true;
	}

	void PhysicsWorld::Impl::FreezeRagdoll(RagdollRecord& record)
	{
		record.FrozenWorld.clear();
		const auto& bodies = System->GetBodyInterface();
		for (size_t i = 0; i < record.Nodes.size(); i++)
		{
			JPH::RVec3 position;
			JPH::Quat rotation;
			bodies.GetPositionAndRotation(record.Doll->GetBodyID(static_cast<int>(i)), position, rotation);
			record.FrozenWorld.push_back(Math::ComposeTransform(FromJolt(position), FromJolt(rotation), glm::vec3(record.Scales[i])));
		}
	}

	void PhysicsWorld::Impl::StopRagdollBodies(UUID uuid, RagdollRecord& record)
	{
		if (!record.Doll)
			return;
		for (auto id : record.Doll->GetBodyIDs())
		{
			BodyToEntity.erase(id.GetIndexAndSequenceNumber());
			ContactListener.Materials[id.GetIndex()] = {};
		}
		record.Doll->RemoveFromPhysicsSystem();
		record.Doll = nullptr;
		DirtyEntities.Insert(uuid);
		EndContacts(uuid, false);
	}

	void PhysicsWorld::Impl::RemoveRagdoll(UUID uuid)
	{
		auto it = Ragdolls.find(uuid);
		if (it == Ragdolls.end())
			return;
		// Erase before dispatching callbacks: they may remove the component/entity again.
		RagdollRecord record = std::move(it->second);
		Ragdolls.erase(it);
		if (auto* animations = ScenePtr->GetAnimationSystem())
			animations->ClearRagdollPose(ScenePtr->GetEntityByUUID(uuid));
		StopRagdollBodies(uuid, record);
	}

	bool PhysicsWorld::ActivateRagdoll(Entity entity, const glm::vec3& impulse)
	{
		auto& impl = *m_Impl;
		if (!impl.Owns(entity) || !entity.HasComponent<RagdollComponent>() || !IsFiniteVec(impulse))
		{
			BS_CORE_ERROR("ActivateRagdoll requires an owned entity with Ragdoll and a finite impulse");
			return false;
		}
		const UUID uuid = entity.GetUUID();
		if (auto it = impl.Ragdolls.find(uuid); it != impl.Ragdolls.end() && it->second.Doll)
		{
			entity.GetComponent<RagdollComponent>().Enabled = true;
			// AddImpulse applies to each body. Divide so the public argument is the total impulse.
			it->second.Doll->AddImpulse(ToJolt(impulse / static_cast<float>(it->second.Nodes.size())));
			return true;
		}
		Impl::RagdollRecord record;
		std::string error;
		if (!impl.BuildRagdoll(entity, record, error))
		{
			impl.RagdollWarnings.Report(entity, { error });
			return false;
		}
		const glm::vec3 velocity = GetLinearVelocity(entity);
		impl.RemoveRagdoll(uuid);
		// Publish before ending old contacts, so reentrant destruction can find and remove the new ragdoll.
		record.Doll->AddToPhysicsSystem(JPH::EActivation::Activate);
		record.Doll->SetLinearVelocity(ToJolt(velocity));
		for (auto id : record.Doll->GetBodyIDs())
			impl.BodyToEntity[id.GetIndexAndSequenceNumber()] = uuid;
		impl.Ragdolls.emplace(uuid, std::move(record));
		impl.RemoveBody(uuid);
		impl.RemoveCharacter(uuid);
		impl.RootMotion.erase(uuid);
		if (!entity || !entity.HasComponent<RagdollComponent>() || !impl.Ragdolls.contains(uuid))
			return false;
		entity.GetComponent<RagdollComponent>().Enabled = true;
		impl.Ragdolls.at(uuid).Doll->AddImpulse(ToJolt(impulse / static_cast<float>(impl.Ragdolls.at(uuid).Nodes.size())));
		impl.RagdollWarnings.Report(entity, {});
		return true;
	}

	bool PhysicsWorld::RecoverRagdoll(Entity entity, const std::string& clip, float seconds)
	{
		auto& impl = *m_Impl;
		if (!impl.Owns(entity) || !entity.HasComponent<RagdollComponent>() || !std::isfinite(seconds) || seconds < 0 || seconds > 60 || !IsRagdollActive(entity))
		{
			BS_CORE_ERROR("RecoverRagdoll requires an active ragdoll and 0..60 finite seconds");
			return false;
		}
		auto* animations = impl.ScenePtr->GetAnimationSystem();
		if (!clip.empty() && (!animations || !animations->Play(entity, clip)))
			return false;
		const UUID uuid = entity.GetUUID();
		auto& record = impl.Ragdolls.at(uuid);
		impl.FreezeRagdoll(record);
		// Keep the get-up at the fall's horizontal position. Height is authored by gameplay/the get-up clip.
		const glm::vec3 delta = glm::vec3(record.FrozenWorld.front()[3]) - record.InitialRoot;
		glm::mat4 world = impl.ScenePtr->GetWorldTransform(entity);
		world[3] += glm::vec4(delta.x, 0, delta.z, 0);
		impl.ScenePtr->SetWorldTransform(entity, world);
		record.World = world;
		entity.GetComponent<RagdollComponent>().Enabled = false;
		record.RecoveryLeft = record.RecoveryDuration = seconds;
		impl.StopRagdollBodies(uuid, record);
		return true;
	}

	void PhysicsWorld::Impl::SyncRagdolls(PhysicsWorld& world, float seconds)
	{
		std::vector<UUID> remove;
		for (auto& [id, record] : Ragdolls)
		{
			Entity entity = ScenePtr->GetEntityByUUID(id);
			const auto* component = entity ? entity.TryGetComponent<RagdollComponent>() : nullptr;
			const auto* animator = entity ? entity.TryGetComponent<AnimatorComponent>() : nullptr;
			if (!component || !animator)
			{
				remove.push_back(id);
				continue;
			}
			glm::vec3 position, scale, previousPosition, previousScale;
			glm::quat rotation, previousRotation;
			const glm::mat4 currentWorld = ScenePtr->GetWorldTransform(entity);
			if (!UniformTransform(currentWorld, position, rotation, scale) || !UniformTransform(record.World, previousPosition, previousRotation, previousScale) || std::abs(scale.x - previousScale.x) > 1e-4f * previousScale.x)
			{
				remove.push_back(id);
				continue;
			}
			if (currentWorld != record.World)
			{
				const glm::mat4 delta = currentWorld * glm::inverse(record.World);
				if (record.Doll)
					FreezeRagdoll(record);
				for (auto& matrix : record.FrozenWorld)
					matrix = delta * matrix;
				if (record.Doll)
				{
					auto& bodies = System->GetBodyInterface();
					const glm::quat turn = rotation * glm::inverse(previousRotation);
					for (size_t i = 0; i < record.Nodes.size(); i++)
					{
						glm::vec3 p, s;
						glm::quat q;
						Math::DecomposeTransform(record.FrozenWorld[i], p, q, s);
						const auto body = record.Doll->GetBodyID(static_cast<int>(i));
						bodies.SetPositionAndRotation(body, ToJolt(p), ToJolt(q), JPH::EActivation::Activate);
						bodies.SetLinearAndAngularVelocity(body, ToJolt(turn * FromJolt(bodies.GetLinearVelocity(body))), ToJolt(turn * FromJolt(bodies.GetAngularVelocity(body))));
					}
				}
				record.InitialRoot = glm::vec3(delta * glm::vec4(record.InitialRoot, 1));
				record.World = currentWorld;
			}
			auto settings = *component;
			settings.Enabled = record.Settings.Enabled;
			settings.RecoveryTime = record.Settings.RecoveryTime;
			auto* animations = ScenePtr->GetAnimationSystem();
			const auto view = animations ? animations->GetPoseView(entity) : std::nullopt;
			if (settings != record.Settings || !view || view->Source != record.Source)
			{
				remove.push_back(id);
				continue;
			}
			if (!record.Doll && std::isfinite(seconds) && seconds > 0)
				record.RecoveryLeft = std::max(0.0f, record.RecoveryLeft - seconds);
			if (!record.Doll && record.RecoveryLeft <= 0)
				remove.push_back(id);
		}
		for (UUID id : remove)
			RemoveRagdoll(id);
		// Registry order makes creation/activation order independent of hash-map iteration.
		for (auto handle : ScenePtr->GetAllEntitiesWith<RagdollComponent>())
		{
			Entity entity(handle, ScenePtr);
			const auto& component = entity.GetComponent<RagdollComponent>();
			if (component.Enabled && !world.IsRagdollActive(entity))
				world.ActivateRagdoll(entity);
			else if (!component.Enabled && world.IsRagdollActive(entity))
				world.RecoverRagdoll(entity, {}, component.RecoveryTime);
		}
	}

	bool PhysicsWorld::IsRagdollActive(Entity entity) const
	{
		if (!m_Impl->Owns(entity))
			return false;
		const auto it = m_Impl->Ragdolls.find(entity.GetUUID());
		return it != m_Impl->Ragdolls.end() && it->second.Doll != nullptr;
	}

	bool PhysicsWorld::HasRagdollPose(Entity entity) const
	{
		return m_Impl->Owns(entity) && m_Impl->Ragdolls.contains(entity.GetUUID());
	}

	uint32_t PhysicsWorld::GetRagdollBodyCount(Entity entity) const
	{
		return IsRagdollActive(entity) ? static_cast<uint32_t>(m_Impl->Ragdolls.at(entity.GetUUID()).Doll->GetBodyCount()) : 0;
	}

	std::vector<RagdollCapsule> PhysicsWorld::GetRagdollCapsules(Entity entity) const
	{
		std::vector<RagdollCapsule> capsules;
		if (!IsRagdollActive(entity))
			return capsules;
		const auto& record = m_Impl->Ragdolls.at(entity.GetUUID());
		const auto& bodies = m_Impl->System->GetBodyInterface();
		for (size_t i = 0; i < record.Capsules.size(); i++)
		{
			JPH::RVec3 p;
			JPH::Quat q;
			bodies.GetPositionAndRotation(record.Doll->GetBodyID(static_cast<int>(i)), p, q);
			const auto& local = record.Capsules[i];
			capsules.push_back({ FromJolt(p) + FromJolt(q) * local.Start, FromJolt(p) + FromJolt(q) * local.End, local.Radius });
		}
		return capsules;
	}

	bool PhysicsWorld::GetRagdollPose(Entity entity, const MeshSource& source, std::vector<std::pair<uint32_t, glm::mat4>>& joints, float& weight) const
	{
		if (!HasRagdollPose(entity))
			return false;
		auto& record = m_Impl->Ragdolls.at(entity.GetUUID());
		if (record.Source.get() != &source)
			return false;
		const glm::mat4 world = m_Impl->ScenePtr->GetWorldTransform(entity);
		glm::vec3 position, scale;
		glm::quat rotation;
		if (!UniformTransform(world, position, rotation, scale))
			return false;
		if (record.Doll)
			m_Impl->FreezeRagdoll(record);
		weight = record.Doll ? 1.0f : (record.RecoveryDuration > 0 ? record.RecoveryLeft / record.RecoveryDuration : 0.0f);
		joints.clear();
		const glm::mat4 inverse = glm::inverse(world);
		for (size_t i = 0; i < record.Nodes.size(); i++)
			joints.emplace_back(record.Nodes[i], inverse * record.FrozenWorld[i]);
		return true;
	}

}
