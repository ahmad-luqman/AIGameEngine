#include "Basalt/Physics/PhysicsWorld.h"

#include "Basalt/Animation/AnimationSystem.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Physics/PhysicsWorldImpl.h"
#include "Basalt/Physics/RagdollRules.h"

#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace Basalt {

	using namespace PhysicsInternal;

	namespace {
		bool UniformTransform(const glm::mat4& matrix, glm::vec3& position, glm::quat& rotation, glm::vec3& scale)
		{
			return DecomposeRagdollTransform(matrix, position, rotation, scale) && scale.x > 1e-5f &&
				   std::abs(scale.y - scale.x) <= 1e-4f * scale.x && std::abs(scale.z - scale.x) <= 1e-4f * scale.x;
		}
	}

	bool PhysicsWorld::Impl::BuildRagdoll(Entity entity, RagdollRecord& record, std::string& error, bool& cacheFailure)
	{
		RagdollBuilds++;
		cacheFailure = true;
		const auto& component = entity.GetComponent<RagdollComponent>();
		error = ValidateRagdoll(component);
		if (!error.empty())
			return false;
		auto* animations = ScenePtr->GetAnimationSystem();
		const auto view = animations ? animations->GetPoseView(entity) : std::nullopt;
		if (!view)
		{
			cacheFailure = false;
			error = entity.HasComponent<AnimatorComponent>() ? "Animator model could not be loaded" : "requires an Animator component";
			return false;
		}
		record.Source = view->Source;
		const auto& source = *view->Source;
		const auto layer = Layers.Find(component.Layer);
		if (!layer)
		{
			error = "unknown Layer '" + component.Layer + "'";
			return false;
		}
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
			if (selected.empty())
				error = "model has no selected skin joints";
			else if (selected.size() > RagdollComponent::MaxBones)
				error = "selected skeleton exceeds " + std::to_string(RagdollComponent::MaxBones) + " bodies";
			else
				error = "RootJoint must name a skin joint";
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
			cacheFailure = false;
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
			if (!UniformTransform(rest[node], restPosition, restRotation, restScale))
			{
				error = "joint '" + source.Nodes[node].Name + "' requires positive uniform rest scale without shear";
				return false;
			}
			if (!UniformTransform(world * rest[node], restPosition, restRotation, restScale) ||
				!UniformTransform(world * view->Globals[node], position, rotation, scale) || std::abs(scale.x - restScale.x) > 1e-4f * restScale.x)
			{
				cacheFailure = false;
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
				cacheFailure = false;
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
			cacheFailure = false;
			error = "could not stabilize the ragdoll constraints";
			return false;
		}
		settings->CalculateConstraintPriorities();
		settings->CalculateBodyIndexToConstraintIndex();
		settings->CalculateConstraintIndexToBodyIdxPair();
		record.Doll = settings->CreateRagdoll(NextRagdollGroup++, static_cast<uint64_t>(entity.GetUUID()), System.get());
		if (!record.Doll)
		{
			cacheFailure = false;
			error = "Jolt body capacity exhausted";
			return false;
		}
		record.World = world;
		record.Settings = component;
		record.Doll->SetPose(JPH::RVec3::sZero(), initial.data());
		return true;
	}

	bool PhysicsWorld::Impl::ReadRagdollWorld(const RagdollRecord& record, std::vector<glm::mat4>& matrices) const
	{
		matrices.clear();
		if (!record.Doll)
			matrices = record.FrozenWorld;
		else
		{
			matrices.reserve(record.Nodes.size());
			const auto& bodies = System->GetBodyInterface();
			for (size_t i = 0; i < record.Nodes.size(); i++)
			{
				JPH::RVec3 position;
				JPH::Quat rotation;
				const auto body = record.Doll->GetBodyID(static_cast<int>(i));
				bodies.GetPositionAndRotation(body, position, rotation);
				if (!Math::IsFinite(FromJolt(position)) || !Math::IsFinite(FromJolt(rotation)) ||
					!IsFiniteVec(FromJolt(bodies.GetLinearVelocity(body))) || !IsFiniteVec(FromJolt(bodies.GetAngularVelocity(body))))
					return false;
				matrices.push_back(Math::ComposeTransform(FromJolt(position), FromJolt(rotation), glm::vec3(record.Scales[i])));
			}
		}
		return matrices.size() == record.Nodes.size() && std::ranges::all_of(matrices, [](const glm::mat4& matrix) { return Math::IsFinite(matrix); });
	}

	std::vector<PhysicsWorld::Impl::RagdollMotion> PhysicsWorld::Impl::CaptureRagdollMotion(const RagdollRecord& record) const
	{
		std::vector<RagdollMotion> motion;
		std::vector<glm::mat4> matrices;
		if (!ReadRagdollWorld(record, matrices))
			return motion;
		const auto& bodies = System->GetBodyInterface();
		for (size_t i = 0; i < record.Nodes.size(); i++)
		{
			RagdollMotion part;
			part.Joint = record.Source->Nodes[record.Nodes[i]].Name;
			glm::vec3 scale;
			Math::DecomposeTransform(matrices[i], part.Position, part.Rotation, scale);
			if (record.Doll)
			{
				const auto body = record.Doll->GetBodyID(static_cast<int>(i));
				JPH::RVec3 p;
				JPH::Quat q;
				bodies.GetPositionAndRotation(body, p, q);
				part.Position = FromJolt(p);
				part.Rotation = FromJolt(q);
				part.Center = FromJolt(bodies.GetCenterOfMassPosition(body));
				part.Linear = FromJolt(bodies.GetLinearVelocity(body));
				part.Angular = FromJolt(bodies.GetAngularVelocity(body));
			}
			else
				part.Center = part.Position;
			motion.push_back(std::move(part));
		}
		return motion;
	}

	void PhysicsWorld::Impl::SeedRagdoll(RagdollRecord& record, const std::vector<RagdollMotion>& motion)
	{
		if (motion.empty())
			return;
		const auto& bodies = System->GetBodyInterface();
		std::vector<glm::vec3> animatedPositions;
		std::vector<glm::quat> animatedRotations;
		for (auto id : record.Doll->GetBodyIDs())
		{
			JPH::RVec3 p;
			JPH::Quat q;
			bodies.GetPositionAndRotation(id, p, q);
			animatedPositions.push_back(FromJolt(p));
			animatedRotations.push_back(FromJolt(q));
		}
		std::vector<glm::vec3> positions;
		std::vector<glm::quat> rotations;
		std::vector<JPH::Mat44> matrices;
		std::vector<bool> used(motion.size(), false);
		for (size_t i = 0; i < record.Nodes.size(); i++)
		{
			const std::string& name = record.Source->Nodes[record.Nodes[i]].Name;
			const int parent = record.Doll->GetRagdollSettings()->GetSkeleton()->GetJoint(static_cast<int>(i)).mParentJointIndex;
			const RagdollMotion* previous = nullptr;
			for (size_t j = 0; j < motion.size(); j++)
				if (!used[j] && motion[j].Joint == name)
				{
					previous = &motion[j];
					used[j] = true;
					break;
				}
			const glm::quat inherited = parent < 0 ? motion.front().Rotation : rotations[parent] * glm::inverse(animatedRotations[parent]) * animatedRotations[i];
			const glm::quat rotation = previous ? previous->Rotation : inherited;
			const glm::vec3 position = parent < 0 ? (previous ? previous->Position : motion.front().Position) : positions[parent] + rotations[parent] * (glm::inverse(animatedRotations[parent]) * (animatedPositions[i] - animatedPositions[parent]));
			positions.push_back(position);
			rotations.push_back(rotation);
			matrices.push_back(JPH::Mat44::sRotationTranslation(ToJolt(rotation), ToJolt(position)));
		}
		record.Doll->SetPose(JPH::RVec3::sZero(), matrices.data());
	}

	void PhysicsWorld::Impl::StopRagdollBodies(UUID uuid, RagdollRecord& record, bool deferContacts)
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
		if (!ShuttingDown)
		{
			DirtyEntities.Insert(uuid);
			EndContacts(uuid, false, deferContacts);
			// Contact callbacks may erase record. Do not access it after dispatch.
		}
	}

	void PhysicsWorld::Impl::RemoveRagdoll(UUID uuid, bool deferContacts)
	{
		auto it = Ragdolls.find(uuid);
		if (it == Ragdolls.end())
			return;
		// Erase before dispatching callbacks: they may remove the component/entity again.
		RagdollRecord record = std::move(it->second);
		Ragdolls.erase(it);
		if (auto* animations = ScenePtr->GetAnimationSystem())
			animations->ClearRagdollPose(ScenePtr->GetEntityByUUID(uuid));
		StopRagdollBodies(uuid, record, deferContacts);
	}

	void PhysicsWorld::Impl::OnRagdollDestroyed(entt::registry& registry, entt::entity handle)
	{
		if (const auto* id = registry.try_get<IDComponent>(handle))
		{
			const UUID uuid = id->ID;
			const bool blocked = UnavailableRagdolls.insert(uuid).second;
			RemoveRagdoll(uuid, true);
			RagdollFailures.erase(uuid);
			RagdollWarnings.Forget(uuid);
			if (blocked)
				UnavailableRagdolls.erase(uuid);
		}
	}

	void PhysicsWorld::Impl::DisableRagdoll(UUID uuid, const char* reason)
	{
		Entity entity = ScenePtr->GetEntityByUUID(uuid);
		if (!entity)
			return;
		BS_CORE_ERROR("Ragdoll '{}': {}; disabling physics without applying its pose", entity.GetName(), reason);
		if (auto* component = entity.TryGetComponent<RagdollComponent>())
			component->Enabled = false;
		const bool blocked = UnavailableRagdolls.insert(uuid).second;
		RemoveRagdoll(uuid);
		if (blocked)
			UnavailableRagdolls.erase(uuid);
	}

	void PhysicsWorld::Impl::ValidateRagdollOutput()
	{
		std::vector<UUID> invalid;
		std::vector<glm::mat4> matrices;
		for (const auto& [uuid, record] : Ragdolls)
			if (!ReadRagdollWorld(record, matrices))
				invalid.push_back(uuid);
		for (UUID uuid : invalid)
			DisableRagdoll(uuid, "non-finite physics output");
	}

	bool PhysicsWorld::ActivateRagdoll(Entity entity, const glm::vec3& impulse)
	{
		return m_Impl->ActivateRagdoll(*this, entity, impulse);
	}

	bool PhysicsWorld::Impl::ActivateRagdoll(PhysicsWorld& world, Entity entity, const glm::vec3& impulse, const std::vector<RagdollMotion>* motion)
	{
		if (!Owns(entity))
		{
			BS_CORE_ERROR("ActivateRagdoll: entity is invalid or belongs to another scene");
			return false;
		}
		const UUID uuid = entity.GetUUID();
		auto reject = [&](const std::string& reason) { RagdollWarnings.Report(entity, { "ActivateRagdoll: " + reason }); return false; };
		if (ShuttingDown || UnavailableRagdolls.contains(uuid) || ScenePtr->IsEntityPendingDestruction(entity))
			return reject("entity or rig is being destroyed");
		if (!entity.HasComponent<RagdollComponent>())
			return reject("requires a Ragdoll component");
		if (!IsFiniteVec(impulse))
			return reject("impulse must be finite");
		auto validateImpulse = [&](const RagdollRecord& record) {
			const glm::vec3 perPart = impulse / static_cast<float>(record.Nodes.size());
			// Jolt clamps speed by its squared length. Reject an input that could overflow that intermediate.
			const double safeSpeed = std::sqrt(static_cast<double>(std::numeric_limits<float>::max())) / 8;
			for (const auto& part : record.Doll->GetRagdollSettings()->mParts)
				if (static_cast<double>(std::max({ std::abs(perPart.x), std::abs(perPart.y), std::abs(perPart.z) })) / part.mMassPropertiesOverride.mMass > safeSpeed)
					return reject("impulse would overflow physics velocity");
			return true;
		};
		if (auto it = Ragdolls.find(uuid); it != Ragdolls.end() && it->second.Doll)
		{
			if (!validateImpulse(it->second))
				return false;
			it->second.Doll->AddImpulse(ToJolt(impulse / static_cast<float>(it->second.Nodes.size())));
			entity.GetComponent<RagdollComponent>().Enabled = true;
			return true;
		}
		RagdollRecord record;
		std::string error;
		bool cacheFailure = false;
		if (!BuildRagdoll(entity, record, error, cacheFailure))
		{
			RagdollWarnings.Report(entity, { error });
			if (cacheFailure)
				RagdollFailures[uuid] = { entity.GetComponent<RagdollComponent>(), record.Source, record.Source != nullptr };
			return false;
		}
		RagdollFailures.erase(uuid);
		if (!validateImpulse(record))
			return false;
		if (motion)
			SeedRagdoll(record, *motion);
		const auto& bodies = System->GetBodyInterface();
		const auto* original = FindBody(entity);
		const glm::vec3 linear = world.GetLinearVelocity(entity);
		const glm::vec3 angular = original ? FromJolt(bodies.GetAngularVelocity(original->ID)) : glm::vec3(0);
		const glm::vec3 center = original ? FromJolt(bodies.GetCenterOfMassPosition(original->ID)) : glm::vec3(ScenePtr->GetWorldTransform(entity)[3]);
		// Complete the state before contact-end callbacks. A retained reference identifies this exact activation
		// if a callback recovers, replaces, removes or destroys it, without dereferencing a cancelled record.
		const JPH::Ref<JPH::Ragdoll> created = record.Doll;
		std::vector<std::pair<glm::vec3, glm::vec3>> velocities;
		std::vector<bool> used(motion ? motion->size() : 0, false);
		for (size_t i = 0; i < record.Nodes.size(); i++)
		{
			const auto body = created->GetBodyID(static_cast<int>(i));
			glm::vec3 v = linear + glm::cross(angular, FromJolt(bodies.GetCenterOfMassPosition(body)) - center);
			glm::vec3 w = angular;
			if (motion && !motion->empty())
			{
				const RagdollMotion* previous = &motion->front();
				for (size_t j = 0; j < motion->size(); j++)
					if (!used[j] && (*motion)[j].Joint == record.Source->Nodes[record.Nodes[i]].Name)
					{
						previous = &(*motion)[j];
						used[j] = true;
						break;
					}
				v = previous->Linear + glm::cross(previous->Angular, FromJolt(bodies.GetCenterOfMassPosition(body)) - previous->Center);
				w = previous->Angular;
			}
			const double safeSpeed = std::sqrt(static_cast<double>(std::numeric_limits<float>::max())) / 8;
			if (!IsFiniteVec(v) || !IsFiniteVec(w) || std::max({ std::abs(v.x), std::abs(v.y), std::abs(v.z), std::abs(w.x), std::abs(w.y), std::abs(w.z) }) > safeSpeed)
			{
				return reject("inherited velocity would overflow physics");
			}
			velocities.emplace_back(v, w);
		}
		// An existing record here is recovery-only: removing it cannot dispatch contact callbacks.
		BS_CORE_ASSERT(!Ragdolls.contains(uuid) || !Ragdolls.at(uuid).Doll, "Active ragdoll already handled");
		RemoveRagdoll(uuid);
		created->AddToPhysicsSystem(JPH::EActivation::Activate);
		for (size_t i = 0; i < velocities.size(); i++)
		{
			const auto body = created->GetBodyID(static_cast<int>(i));
			System->GetBodyInterface().SetLinearAndAngularVelocity(body, ToJolt(velocities[i].first), ToJolt(velocities[i].second));
			BodyToEntity[body.GetIndexAndSequenceNumber()] = uuid;
		}
		created->AddImpulse(ToJolt(impulse / static_cast<float>(record.Nodes.size())));
		entity.GetComponent<RagdollComponent>().Enabled = true;
		const bool inserted = Ragdolls.emplace(uuid, std::move(record)).second;
		BS_CORE_ASSERT(inserted, "Activation must replace only an erased recovery");
		(void)inserted;
		RemoveBody(uuid);
		RemoveCharacter(uuid);
		RootMotion.erase(uuid);
		const auto current = Ragdolls.find(uuid);
		if (!entity || ScenePtr->IsEntityPendingDestruction(entity) || current == Ragdolls.end() || current->second.Doll != created || !entity.HasComponent<RagdollComponent>())
		{
			if (entity)
				RagdollWarnings.Report(entity, { "activation was cancelled by a contact callback" });
			else
				BS_CORE_WARN("ActivateRagdoll: entity {} was destroyed by a contact callback", static_cast<uint64_t>(uuid));
			return false;
		}
		RagdollWarnings.Report(entity, {});
		return true;
	}

	bool PhysicsWorld::RecoverRagdoll(Entity entity, const std::string& clip, float seconds)
	{
		auto& impl = *m_Impl;
		if (!impl.Owns(entity))
		{
			BS_CORE_ERROR("RecoverRagdoll: entity is invalid or belongs to another scene");
			return false;
		}
		auto reject = [&](const std::string& reason) { impl.RagdollWarnings.Report(entity, { "RecoverRagdoll: " + reason }); return false; };
		if (!std::isfinite(seconds) || seconds < 0 || seconds > RagdollComponent::MaxRecoveryTime)
			return reject(fmt::format("duration must be finite and in 0..{} seconds", RagdollComponent::MaxRecoveryTime));
		if (!entity.HasComponent<RagdollComponent>() || !IsRagdollActive(entity))
			return reject("requires an active Ragdoll component");
		const UUID uuid = entity.GetUUID();
		auto& record = impl.Ragdolls.at(uuid);
		auto* animations = impl.ScenePtr->GetAnimationSystem();
		const auto view = animations ? animations->GetPoseView(entity) : std::nullopt;
		if (!view || view->Source != record.Source)
			return reject("Animator model changed; update physics before recovering");
		glm::mat4 world = impl.ScenePtr->GetWorldTransform(entity);
		glm::vec3 position, scale;
		glm::quat rotation;
		if (!UniformTransform(world, position, rotation, scale))
			return reject("requires a positive uniform world transform without shear");
		std::vector<glm::mat4> frozen;
		if (!impl.ReadRagdollWorld(record, frozen))
		{
			impl.DisableRagdoll(uuid, "non-finite physics output during recovery");
			return false;
		}
		std::vector<glm::mat4> target;
		if (!animations->GetAnimatedPose(entity, *record.Source, target, clip))
			return reject("get-up clip is missing or has no finite pose");
		// Match the incoming clip's root, including authored rest offsets and animated root displacement.
		// Gameplay supplies height for a different ground level; the blend keeps the physical pose at t=0.
		const glm::vec3 targetRoot = glm::vec3(world * target[record.Nodes.front()] * glm::vec4(0, 0, 0, 1));
		if (!Math::IsFinite(targetRoot))
			return reject("get-up root is non-finite in world space");
		const glm::vec3 delta = glm::vec3(frozen.front()[3]) - targetRoot;
		world[3] += glm::vec4(delta.x, 0, delta.z, 0);
		if (!Math::IsFinite(world))
			return reject("get-up placement is non-finite");
		if (!clip.empty() && !animations->Play(entity, clip))
			return false;
		const JPH::Ref<JPH::Ragdoll> retiring = record.Doll;
		impl.ScenePtr->SetWorldTransform(entity, world);
		record.World = impl.ScenePtr->GetWorldTransform(entity);
		record.FrozenWorld = std::move(frozen);
		entity.GetComponent<RagdollComponent>().Enabled = false;
		record.RecoveryLeft = record.RecoveryDuration = seconds;
		if (seconds == 0)
			impl.RemoveRagdoll(uuid); // Erase before callbacks; never erase a replacement recovery they create.
		else
		{
			// Relocation changes model-space physical transforms. Refresh before callbacks can sample or reactivate.
			animations->RefreshRagdollPoses();
			const auto current = impl.Ragdolls.find(uuid);
			if (entity && current != impl.Ragdolls.end() && current->second.Doll == retiring &&
				entity.HasComponent<RagdollComponent>() && !entity.GetComponent<RagdollComponent>().Enabled)
				impl.StopRagdollBodies(uuid, current->second);
		}
		animations->RefreshRagdollPoses();
		return true;
	}

	void PhysicsWorld::Impl::SyncRagdolls(PhysicsWorld& world, float seconds)
	{
		std::vector<UUID> remove;
		std::vector<UUID> invalid;
		std::vector<UUID> unsupported;
		std::map<UUID, std::vector<RagdollMotion>> rebuilds;
		for (auto& [id, record] : Ragdolls)
		{
			Entity entity = ScenePtr->GetEntityByUUID(id);
			const auto* component = entity ? entity.TryGetComponent<RagdollComponent>() : nullptr;
			if (!component || !entity.HasComponent<AnimatorComponent>())
			{
				remove.push_back(id);
				continue;
			}
			std::vector<glm::mat4> matrices;
			if (!ReadRagdollWorld(record, matrices))
			{
				invalid.push_back(id);
				continue;
			}
			glm::vec3 position, scale, previousPosition, previousScale;
			glm::quat rotation, previousRotation;
			const glm::mat4 currentWorld = ScenePtr->GetWorldTransform(entity);
			if (!UniformTransform(currentWorld, position, rotation, scale) || !UniformTransform(record.World, previousPosition, previousRotation, previousScale))
			{
				unsupported.push_back(id);
				continue;
			}
			const bool resized = std::abs(scale.x - previousScale.x) > 1e-4f * previousScale.x;
			if (currentWorld != record.World)
			{
				// A live resize keeps the root at its physical position. Recovery is an entity-relative frozen
				// pose, so its scale follows the entity like the animation it is blending toward.
				const glm::mat4 target = record.Doll ? Math::ComposeTransform(position, rotation, previousScale) : currentWorld;
				const glm::mat4 delta = target * glm::inverse(record.World);
				bool finite = true;
				for (auto& matrix : matrices)
				{
					matrix = delta * matrix;
					finite = finite && Math::IsFinite(matrix);
				}
				if (!finite)
				{
					invalid.push_back(id);
					continue;
				}
				if (record.Doll)
				{
					auto& bodies = System->GetBodyInterface();
					const glm::quat turn = rotation * glm::inverse(previousRotation);
					for (size_t i = 0; i < record.Nodes.size(); i++)
					{
						glm::vec3 p, s;
						glm::quat q;
						Math::DecomposeTransform(matrices[i], p, q, s);
						const auto body = record.Doll->GetBodyID(static_cast<int>(i));
						bodies.SetPositionAndRotation(body, ToJolt(p), ToJolt(q), JPH::EActivation::Activate);
						bodies.SetLinearAndAngularVelocity(body, ToJolt(turn * FromJolt(bodies.GetLinearVelocity(body))), ToJolt(turn * FromJolt(bodies.GetAngularVelocity(body))));
					}
				}
				else
					record.FrozenWorld = std::move(matrices);
				record.World = currentWorld;
			}
			auto settings = *component;
			settings.Enabled = record.Settings.Enabled;
			settings.RecoveryTime = record.Settings.RecoveryTime;
			auto* animations = ScenePtr->GetAnimationSystem();
			const auto view = animations ? animations->GetPoseView(entity) : std::nullopt;
			const bool sourceChanged = !view || view->Source != record.Source;
			if (record.Doll && (sourceChanged || (component->Enabled && (resized || settings != record.Settings))))
			{
				rebuilds[id] = CaptureRagdollMotion(record);
				remove.push_back(id);
				continue;
			}
			if (!record.Doll)
			{
				if (sourceChanged)
				{
					const auto motion = CaptureRagdollMotion(record);
					RagdollRecord replacement;
					std::string error;
					bool cacheFailure = false;
					if (!BuildRagdoll(entity, replacement, error, cacheFailure))
					{
						RagdollWarnings.Report(entity, { "recovery stopped after model change: " + error });
						remove.push_back(id);
						continue;
					}
					SeedRagdoll(replacement, motion);
					if (!ReadRagdollWorld(replacement, replacement.FrozenWorld))
					{
						invalid.push_back(id);
						continue;
					}
					// The replacement's bodies were staged, never added to physics.
					replacement.Doll = nullptr;
					replacement.RecoveryLeft = record.RecoveryLeft;
					replacement.RecoveryDuration = record.RecoveryDuration;
					record = std::move(replacement);
				}
				record.Settings = *component;
				if (std::isfinite(seconds) && seconds > 0)
					record.RecoveryLeft = std::max(0.0f, record.RecoveryLeft - seconds);
				if (record.RecoveryLeft <= 0)
					remove.push_back(id);
			}
		}
		for (UUID id : unsupported)
			DisableRagdoll(id, "running rig requires a positive uniform world transform without shear");
		for (UUID id : invalid)
			DisableRagdoll(id, "non-finite physical pose or teleport result");
		// Rebuilds publish their replacement and inherited motion before ended-contact callbacks can reactivate.
		for (UUID id : remove)
			RemoveRagdoll(id, rebuilds.contains(id));
		// Snapshot registry order: callbacks may remove/add components. Stable allocation order pins Jolt body
		// IDs and collision groups across replays, independent of callback-induced pool reallocations.
		std::vector<UUID> order;
		for (auto handle : ScenePtr->GetAllEntitiesWith<RagdollComponent>())
			order.push_back(Entity(handle, ScenePtr).GetUUID());
		for (UUID uuid : order)
		{
			Entity entity = ScenePtr->GetEntityByUUID(uuid);
			const auto* component = entity ? entity.TryGetComponent<RagdollComponent>() : nullptr;
			if (!component)
				continue;
			if ((component->Enabled || rebuilds.contains(uuid)) && !world.IsRagdollActive(entity))
			{
				if (auto failure = RagdollFailures.find(uuid); failure != RagdollFailures.end())
				{
					// RecoveryTime is validated too: editing an invalid duration must permit a new attempt.
					auto settings = *component;
					settings.Enabled = failure->second.Settings.Enabled;
					if (settings == failure->second.Settings)
					{
						if (!failure->second.DependsOnSource)
							continue;
						auto* animations = ScenePtr->GetAnimationSystem();
						const auto view = animations ? animations->GetPoseView(entity) : std::nullopt;
						if (view && view->Source == failure->second.Source.lock())
							continue;
					}
				}
				const auto seed = rebuilds.find(uuid);
				const bool recover = !component->Enabled;
				const float duration = component->RecoveryTime;
				if (ActivateRagdoll(world, entity, glm::vec3(0), seed == rebuilds.end() ? nullptr : &seed->second) && recover)
				{
					if (!world.RecoverRagdoll(entity, {}, duration) && world.IsRagdollActive(entity))
						entity.GetComponent<RagdollComponent>().Enabled = true;
				}
			}
			else if (!component->Enabled && world.IsRagdollActive(entity))
			{
				if (!world.RecoverRagdoll(entity, {}, component->RecoveryTime) && world.IsRagdollActive(entity))
					entity.GetComponent<RagdollComponent>().Enabled = true;
			}
		}
	}

	void PhysicsWorld::RejectRagdollPose(Entity entity)
	{
		if (m_Impl->Owns(entity))
			m_Impl->DisableRagdoll(entity.GetUUID(), "physical pose cannot be expressed in the animated hierarchy");
	}

	bool PhysicsWorld::IsRagdollActive(Entity entity) const
	{
		if (!m_Impl->Owns(entity) || !entity.HasComponent<RagdollComponent>() || !entity.HasComponent<AnimatorComponent>())
			return false;
		const auto it = m_Impl->Ragdolls.find(entity.GetUUID());
		return it != m_Impl->Ragdolls.end() && it->second.Doll != nullptr;
	}

	bool PhysicsWorld::HasRagdollPose(Entity entity) const
	{
		if (!m_Impl->Owns(entity) || !entity.HasComponent<RagdollComponent>() || !entity.HasComponent<AnimatorComponent>())
			return false;
		const auto it = m_Impl->Ragdolls.find(entity.GetUUID());
		return it != m_Impl->Ragdolls.end() && it->second.Weight() > 0;
	}

	uint32_t PhysicsWorld::GetRagdollBodyCount(Entity entity) const
	{
		return IsRagdollActive(entity) ? static_cast<uint32_t>(m_Impl->Ragdolls.at(entity.GetUUID()).Doll->GetBodyCount()) : 0;
	}

	uint64_t PhysicsWorld::GetRagdollBuildCount() const
	{
		return m_Impl->RagdollBuilds;
	}

	std::vector<RagdollCapsule> PhysicsWorld::GetRagdollCapsules(Entity entity) const
	{
		std::vector<RagdollCapsule> capsules;
		if (!IsRagdollActive(entity))
			return capsules;
		const auto& record = m_Impl->Ragdolls.at(entity.GetUUID());
		const auto& bodies = m_Impl->System->GetBodyInterface();
		capsules.reserve(record.Capsules.size());
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

	std::optional<RagdollPose> PhysicsWorld::GetRagdollPose(Entity entity) const
	{
		if (!HasRagdollPose(entity))
			return std::nullopt;
		const auto& record = m_Impl->Ragdolls.at(entity.GetUUID());
		const glm::mat4 world = m_Impl->ScenePtr->GetWorldTransform(entity);
		glm::vec3 position, scale;
		glm::quat rotation;
		if (!UniformTransform(world, position, rotation, scale))
			return std::nullopt;
		std::vector<glm::mat4> matrices;
		if (!m_Impl->ReadRagdollWorld(record, matrices))
			return std::nullopt;
		RagdollPose pose;
		pose.Source = record.Source;
		pose.Weight = record.Weight();
		pose.Active = record.Doll != nullptr;
		pose.Sleeping = record.Doll && !record.Doll->IsActive();
		const glm::mat4 inverse = glm::inverse(world);
		for (size_t i = 0; i < record.Nodes.size(); i++)
			pose.Joints.emplace_back(record.Nodes[i], inverse * matrices[i]);
		return pose;
	}

	bool PhysicsWorld::GetRagdollPose(Entity entity, const MeshSource& source, std::vector<std::pair<uint32_t, glm::mat4>>& joints, float& weight) const
	{
		auto pose = GetRagdollPose(entity);
		if (!pose || pose->Source.get() != &source)
			return false;
		joints = std::move(pose->Joints);
		weight = pose->Weight;
		return true;
	}

}
