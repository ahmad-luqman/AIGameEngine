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
		// The most balance lifts a sinking rig, in multiples of gravity's acceleration.
		constexpr float MaxStandAcceleration = 2.0f;
		constexpr float PartFriction = 0.5f;

		// Visits a rig's swing-twist joints until `visit` returns true; whether one did.
		template<typename Visit>
		bool ForEachSwingTwist(JPH::Ragdoll& doll, const Visit& visit)
		{
			for (size_t i = 0; i < doll.GetConstraintCount(); i++)
				if (auto* constraint = doll.GetConstraint(static_cast<int>(i)); constraint && constraint->GetSubType() == JPH::EConstraintSubType::SwingTwist)
					if (visit(*static_cast<JPH::SwingTwistConstraint*>(constraint)))
						return true;
			return false;
		}

		// The parts' mass-weighted height above the lowest part's origin, from each part's height: what balance keeps at the
		// animated pose's value, needing no idea where the ground is.
		float StandingHeight(const JPH::Ragdoll& doll, const std::vector<float>& heights)
		{
			const auto& parts = doll.GetRagdollSettings()->mParts;
			float weighted = 0, mass = 0;
			for (size_t i = 0; i < parts.size(); i++)
			{
				weighted += heights[i] * parts[i].mMassPropertiesOverride.mMass;
				mass += parts[i].mMassPropertiesOverride.mMass;
			}
			return weighted / mass - *std::ranges::min_element(heights);
		}

		// `from` moved `weight` of the way to `to`, both rigid-plus-scale joint transforms; `to` when either is not.
		glm::mat4 PartWay(const glm::mat4& from, const glm::mat4& to, float weight)
		{
			glm::vec3 fromPosition, fromScale, toPosition, toScale;
			glm::quat fromRotation, toRotation;
			if (!DecomposeRagdollTransform(from, fromPosition, fromRotation, fromScale) || !DecomposeRagdollTransform(to, toPosition, toRotation, toScale))
				return to;
			return Math::ComposeTransform(glm::mix(fromPosition, toPosition, weight), Math::Slerp(fromRotation, toRotation, weight), glm::mix(fromScale, toScale, weight));
		}

		// Turns the motor of the joint above part `index` toward `target` (relative to its parent) with up to `limit`: what
		// Ragdoll::DriveToPoseUsingMotors does, with one limit per joint. At 0 the joint is limp.
		void DriveJointMotor(JPH::Ragdoll& doll, int index, const JPH::Quat& target, float limit, float frequency, float damping)
		{
			const int constraint = doll.GetRagdollSettings()->GetConstraintIndexForBodyIndex(index);
			if (constraint < 0 || doll.GetConstraint(constraint)->GetSubType() != JPH::EConstraintSubType::SwingTwist)
				return;
			auto& joint = *static_cast<JPH::SwingTwistConstraint*>(doll.GetConstraint(constraint));
			for (JPH::MotorSettings* motor : { &joint.GetSwingMotorSettings(), &joint.GetTwistMotorSettings() })
			{
				motor->mSpringSettings.mMode = JPH::ESpringMode::FrequencyAndDamping;
				motor->mSpringSettings.mFrequency = frequency;
				motor->mSpringSettings.mDamping = damping;
				motor->SetTorqueLimit(limit);
			}
			const JPH::EMotorState state = limit > 0 ? JPH::EMotorState::Position : JPH::EMotorState::Off;
			joint.SetSwingMotorState(state);
			joint.SetTwistMotorState(state);
			if (limit > 0)
				joint.SetTargetOrientationBS(target);
		}

		bool UniformTransform(const glm::mat4& matrix, glm::vec3& position, glm::quat& rotation, glm::vec3& scale)
		{
			return DecomposeRagdollTransform(matrix, position, rotation, scale) && scale.x > 1e-5f &&
				   std::abs(scale.y - scale.x) <= 1e-4f * scale.x && std::abs(scale.z - scale.x) <= 1e-4f * scale.x;
		}
	}

	bool PhysicsWorld::Impl::RagdollReplacesBody(UUID uuid) const
	{
		const auto it = Ragdolls.find(uuid);
		return it != Ragdolls.end() && it->second.Doll && it->second.OwnsRoot();
	}

	UUID PhysicsWorld::Impl::FindRagdollCharacter(Entity entity) const
	{
		for (Entity holder = entity; holder; holder = holder.GetParent())
			if (Characters.contains(holder.GetUUID()))
				return holder.GetUUID();
		return 0;
	}

	bool PhysicsWorld::Impl::IsRagdollPartOf(const JPH::Body& body, UUID character) const
	{
		const auto it = Ragdolls.find(UUID(body.GetUserData()));
		return character != 0 && it != Ragdolls.end() && it->second.Live.Character == character && it->second.HasPart(body.GetID());
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
		const std::set<uint32_t> selected = SelectRagdollJoints(source, root);
		const bool rootSelected = root < 0 || selected.contains(static_cast<uint32_t>(root));
		const bool onlyRoot = component.KinematicRoot && selected.size() == 1;
		if (selected.empty() || selected.size() > RagdollComponent::MaxBones || !rootSelected || onlyRoot)
		{
			if (selected.empty())
				error = "model has no selected skin joints";
			else if (onlyRoot && rootSelected)
				error = "KinematicRoot leaves nothing to simulate: no skin joint hangs from the root";
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
			part.mMotionType = component.KinematicRoot && parentIndex < 0 ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic;
			part.mObjectLayer = MakeObjectLayer(true, *layer);
			part.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
			part.mMassPropertiesOverride.mMass = bone.Mass > 0 ? bone.Mass : component.Mass / static_cast<float>(selected.size());
			part.mFriction = PartFriction;
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
		for (const RagdollBone& bone : component.Bones)
			record.BoneParts.push_back(indices.at(static_cast<uint32_t>(FindNode(source, bone.Joint))));
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
		const std::vector<JPH::BodyID> parts(record.Doll->GetBodyIDs().begin(), record.Doll->GetBodyIDs().end());
		for (const JPH::BodyID id : parts)
		{
			BodyToEntity.erase(id.GetIndexAndSequenceNumber());
			ContactListener.Materials[id.GetIndex()] = {};
		}
		record.Doll->RemoveFromPhysicsSystem();
		record.Doll = nullptr;
		const bool hadCharacter = record.Live.Character != 0;
		record.Live = {};
		if (!ShuttingDown)
		{
			if (hadCharacter)
				UpdateIgnoredPairs();
			// A rig that left its entity's body in place has nothing to restore.
			if (record.OwnsRoot())
				DirtyEntities.Insert(uuid);
			// The body the entity keeps may be a trigger, which makes its limbs' contacts triggers.
			EndContacts(uuid, parts, IsTrigger(uuid), deferContacts);
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
			MotorWarnings.Forget(uuid);
			MotorCallWarnings.Forget(uuid);
			RagdollStrength.erase(uuid);
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
		// A kinematic root takes no impulse, so the total goes to the parts that do.
		const auto impulseShare = [&](const RagdollRecord& record) { return impulse / static_cast<float>(record.Nodes.size() - (record.OwnsRoot() ? 0 : 1)); };
		auto validateImpulse = [&](const RagdollRecord& record) {
			const glm::vec3 perPart = impulseShare(record);
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
			it->second.Doll->AddImpulse(ToJolt(impulseShare(it->second)));
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
		// A rig on a character's child moves with that character.
		record.Live.Character = record.OwnsRoot() ? UUID(0) : FindRagdollCharacter(entity);
		const glm::vec3 linear = world.GetLinearVelocity(record.Live.Character != 0 ? ScenePtr->GetEntityByUUID(record.Live.Character) : entity);
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
			ContactListener.Materials[body.GetIndex()].TracksContacts = true;
		}
		created->AddImpulse(ToJolt(impulseShare(record)));
		entity.GetComponent<RagdollComponent>().Enabled = true;
		const bool replacesBody = record.OwnsRoot();
		const bool inserted = Ragdolls.emplace(uuid, std::move(record)).second;
		BS_CORE_ASSERT(inserted, "Activation must replace only an erased recovery");
		(void)inserted;
		if (replacesBody)
		{
			RemoveBody(uuid);
			RemoveCharacter(uuid);
			RootMotion.erase(uuid);
		}
		else
			UpdateIgnoredPairs();
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
		// A kinematic root never left the animation, so the entity stays where it is.
		if (record.OwnsRoot())
		{
			// Match the incoming clip's root, including authored rest offsets and animated root displacement.
			// Gameplay supplies height for a different ground level; the blend keeps the physical pose at t=0.
			const glm::vec3 targetRoot = glm::vec3(world * target[record.Nodes.front()] * glm::vec4(0, 0, 0, 1));
			if (!Math::IsFinite(targetRoot))
				return reject("get-up root is non-finite in world space");
			const glm::vec3 delta = glm::vec3(frozen.front()[3]) - targetRoot;
			world[3] += glm::vec4(delta.x, 0, delta.z, 0);
			if (!Math::IsFinite(world))
				return reject("get-up placement is non-finite");
		}
		if (!clip.empty() && !animations->Play(entity, clip))
			return false;
		const JPH::Ref<JPH::Ragdoll> retiring = record.Doll;
		if (record.OwnsRoot())
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
		bool charactersChanged = false;
		for (auto& [id, record] : Ragdolls)
		{
			Entity entity = ScenePtr->GetEntityByUUID(id);
			const auto* component = entity ? entity.TryGetComponent<RagdollComponent>() : nullptr;
			if (!component || !entity.HasComponent<AnimatorComponent>())
			{
				remove.push_back(id);
				continue;
			}
			if (record.Doll && !record.OwnsRoot())
			{
				const UUID character = FindRagdollCharacter(entity);
				charactersChanged = charactersChanged || character != record.Live.Character;
				record.Live.Character = character;
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
			// A kinematic root walks with its entity and what hangs from it follows through the joints, lagging as it should;
			// FollowRagdollRoot carries the rig through a teleport.
			if (record.Doll && !record.OwnsRoot())
				record.World = currentWorld;
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
			auto* animations = ScenePtr->GetAnimationSystem();
			const auto view = animations ? animations->GetPoseView(entity) : std::nullopt;
			const bool sourceChanged = !view || view->Source != record.Source;
			if (record.Doll && (sourceChanged || (component->Enabled && (resized || RagdollNeedsRebuild(record.Settings, *component)))))
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
				if (std::isfinite(seconds) && seconds > 0)
					record.RecoveryLeft = std::max(0.0f, record.RecoveryLeft - seconds);
				if (record.RecoveryLeft <= 0)
					remove.push_back(id);
			}
		}
		if (charactersChanged)
			UpdateIgnoredPairs();
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
		DriveRagdollMotors(seconds);
	}

	FadingWeight& PhysicsWorld::Impl::MotorStrengthFor(UUID uuid, const RagdollComponent& component)
	{
		FadingWeight& strength = RagdollStrength.try_emplace(uuid, component.MotorStrength).first->second.Weight;
		strength.Configure(component.MotorStrength);
		return strength;
	}

	void PhysicsWorld::Impl::DriveRagdollMotors(float seconds)
	{
		// Strengths fade while their rig is inactive too, so a fade a script started before activation has moved on.
		for (auto it = RagdollStrength.begin(); it != RagdollStrength.end();)
		{
			Entity entity = ScenePtr->GetEntityByUUID(it->first);
			const auto* component = entity ? entity.TryGetComponent<RagdollComponent>() : nullptr;
			if (!component)
			{
				it = RagdollStrength.erase(it);
				continue;
			}
			it->second.Weight.Configure(component->MotorStrength);
			if (std::isfinite(seconds) && seconds > 0)
				it->second.Weight.Advance(seconds);
			++it;
		}
		for (auto& [id, record] : Ragdolls)
		{
			if (!record.Doll)
				continue;
			Entity entity = ScenePtr->GetEntityByUUID(id);
			const auto* component = entity ? entity.TryGetComponent<RagdollComponent>() : nullptr;
			if (!component)
				continue;
			ApplyRagdollMotors(entity, record, *component, MotorStrengthFor(id, *component).Get());
		}
	}

	void PhysicsWorld::Impl::ApplyRagdollMotors(Entity entity, RagdollRecord& record, const RagdollComponent& component, float strength)
	{
		std::vector<std::string> problems;
		// C++ code can write values the registry rejects: clamped (NaN to the default), and said so once.
		const auto setting = [&](float value, float low, float high, float fallback, std::string_view name) {
			if (std::isfinite(value) && value >= low && value <= high)
				return value;
			problems.push_back(fmt::format("{} must be in {}..{}; clamping", name, low, high));
			return std::isnan(value) ? fallback : std::clamp(value, low, high);
		};
		const RagdollComponent defaults;
		record.Live.AnimatedRoot.reset();
		record.Live.Balance.reset();
		// The strength itself is already clamped (FadingWeight; a non-finite one is 0): only said so.
		if (!std::isfinite(component.MotorStrength))
			problems.emplace_back("MotorStrength must be finite; motors are off");
		else
			setting(component.MotorStrength, 0.0f, 1.0f, defaults.MotorStrength, "MotorStrength");
		const float frequency = setting(component.MotorFrequency, RagdollComponent::MinMotorFrequency, RagdollComponent::MaxMotorFrequency, defaults.MotorFrequency, "MotorFrequency");
		const float damping = setting(component.MotorDamping, 0.0f, RagdollComponent::MaxMotorDamping, defaults.MotorDamping, "MotorDamping");
		const float torque = setting(component.MotorTorque, 0.0f, RagdollComponent::MaxMotorTorque, defaults.MotorTorque, "MotorTorque") * strength;
		const bool powered = torque > 0;
		const float balance = setting(component.BalanceStrength, 0.0f, 1.0f, defaults.BalanceStrength, "BalanceStrength") * strength;
		const float balanceFrequency = setting(component.BalanceFrequency, RagdollComponent::MinMotorFrequency, RagdollComponent::MaxMotorFrequency, defaults.BalanceFrequency, "BalanceFrequency");
		const float balanceDamping = setting(component.BalanceDamping, 0.0f, RagdollComponent::MaxMotorDamping, defaults.BalanceDamping, "BalanceDamping");
		const float maxTilt = setting(component.BalanceMaxTilt, 0.0f, RagdollComponent::MaxTiltLimit, defaults.BalanceMaxTilt, "BalanceMaxTilt");
		std::vector<float> boneStrength(record.Nodes.size(), 1.0f);
		// Built entries that still name their joint: an edit that moved one waits for its rebuild.
		for (size_t i = 0; i < std::min(component.Bones.size(), record.BoneParts.size()); i++)
			if (component.Bones[i].Joint == record.Settings.Bones[i].Joint)
				boneStrength[record.BoneParts[i]] = setting(component.Bones[i].MotorStrength, 0.0f, 1.0f, 1.0f, fmt::format("Bones[{}] '{}' MotorStrength", i, component.Bones[i].Joint));
		// Never touched while off, so a ragdoll that was never powered simulates exactly like a passive one.
		const auto turnOff = [&] {
			if (record.Live.MotorsOn)
				ForEachSwingTwist(*record.Doll, [](JPH::SwingTwistConstraint& joint) {
					joint.SetSwingMotorState(JPH::EMotorState::Off);
					joint.SetTwistMotorState(JPH::EMotorState::Off);
					return false;
				});
			record.Live.MotorsOn = false;
			MotorWarnings.Report(entity, problems);
		};
		const bool followsRoot = !record.OwnsRoot();
		if (!powered && !followsRoot)
			return turnOff();
		// What a pose this frame cannot provide stops: the motors, what rests on them, and a root that follows.
		std::string stopped = "motors are off";
		if (!powered)
			stopped = "the root holds still";
		else if (followsRoot)
			stopped = "motors are off and the root holds still";
		else if (balance > 0)
			stopped = "motors and balance are off";
		const auto stop = [&](std::string_view reason, std::string_view until = {}) {
			problems.push_back(fmt::format("{}; {}{}", reason, stopped, until));
			turnOff();
		};
		// The layered animation before physical overlays: the pose the body tries to hold.
		auto* animations = ScenePtr->GetAnimationSystem();
		const auto view = animations ? animations->GetPoseView(entity) : std::nullopt;
		if (!view)
			return stop("the Animator cannot animate");
		if (view->Source != record.Source)
			return stop("the Animator's model differs from the one the ragdoll was built for", " until it rebuilds");
		// A pose that does not compose leaves `animated` partly filled: never drive toward it.
		std::vector<glm::mat4> animated;
		if (!animations->GetAnimatedPose(entity, *record.Source, animated))
			return stop("the animated pose is not finite");
		// Each body's rotation relative to its skeleton parent's, from the animated joints in world space. The portable
		// decomposition keeps the targets identical on every platform.
		const glm::mat4 world = ScenePtr->GetWorldTransform(entity);
		glm::vec3 position, scale;
		glm::quat rotation;
		if (!DecomposeRagdollTransform(world, position, rotation, scale))
			return stop("the entity's world transform cannot be decomposed");
		std::vector<glm::quat> rotations(record.Nodes.size());
		std::vector<glm::vec3> positions(record.Nodes.size());
		// The root first: it is all a rig that follows it needs, whatever becomes of the limbs and their motors.
		if (!DecomposeRagdollTransform(world * animated[record.Nodes.front()], positions[0], rotations[0], scale))
			return stop(fmt::format("the animated root '{}' cannot be decomposed", record.Source->Nodes[record.Nodes.front()].Name));
		record.Live.AnimatedRoot = animated[record.Nodes.front()];
		if (!powered)
			return turnOff();
		// Every joint is read before any motor changes, so a joint that cannot be decomposed leaves no rig half driven.
		for (size_t i = 1; i < record.Nodes.size(); i++)
			if (!DecomposeRagdollTransform(world * animated[record.Nodes[i]], positions[i], rotations[i], scale))
			{
				problems.push_back(fmt::format("the animated joint '{}' cannot be decomposed; {}", record.Source->Nodes[record.Nodes[i]].Name, balance > 0 && !followsRoot ? "motors and balance are off" : "motors are off"));
				return turnOff();
			}
		const JPH::Skeleton* skeleton = record.Doll->GetRagdollSettings()->GetSkeleton();
		for (size_t i = 0; i < record.Nodes.size(); i++)
		{
			const int parent = skeleton->GetJoint(static_cast<int>(i)).mParentJointIndex;
			const glm::quat local = parent < 0 ? rotations[i] : glm::conjugate(rotations[static_cast<size_t>(parent)]) * rotations[i];
			DriveJointMotor(*record.Doll, static_cast<int>(i), ToJolt(local).Normalized(), torque * boneStrength[i], frequency, damping);
		}
		// A sleeping body ignores its motors: the animation keeps a powered rig awake.
		record.Doll->Activate();
		record.Live.MotorsOn = true;
		const JPH::Vec3 gravity = System->GetGravity();
		const bool balances = balance > 0 && !followsRoot;
		if (balances && gravity.IsNearZero())
			problems.emplace_back("balance has no gravity to stand against; it is off");
		else if (balances)
		{
			const JPH::Vec3 up = -gravity.Normalized();
			const float angular = 2.0f * JPH::JPH_PI * balanceFrequency;
			std::vector<float> heights;
			heights.reserve(positions.size());
			for (const glm::vec3& joint : positions)
				heights.push_back(up.Dot(ToJolt(joint)));
			record.Live.Balance = RagdollRecord::BalanceDrive{ .Strength = balance, .Stiffness = angular * angular, .Damping = 2.0f * balanceDamping * angular, .CosMaxTilt = JPH::Cos(JPH::DegreesToRadians(maxTilt)), .StandHeight = StandingHeight(*record.Doll, heights) };
		}
		MotorWarnings.Report(entity, problems);
	}

	void PhysicsWorld::Impl::StepRagdolls(float fixedStep, uint32_t stepsLeft)
	{
		for (auto& [id, record] : Ragdolls)
		{
			if (!record.Doll)
				continue;
			const Entity entity = ScenePtr->GetEntityByUUID(id);
			if (!entity)
				continue;
			if (record.OwnsRoot())
				BalanceRagdoll(entity, record, fixedStep);
			else
				FollowRagdollRoot(entity, record, fixedStep, stepsLeft);
		}
	}

	void PhysicsWorld::Impl::FollowRagdollRoot(Entity entity, RagdollRecord& record, float fixedStep, uint32_t stepsLeft)
	{
		JPH::BodyInterface& bodies = System->GetBodyInterface();
		const JPH::BodyID root = record.Doll->GetBodyID(0);
		// A frame's animated move is divided among its steps: the root would take it all in the first and stop in the rest.
		std::optional<glm::mat4> target = record.Live.AnimatedRoot;
		if (target && record.Live.FollowedRoot && stepsLeft > 1)
			target = PartWay(*record.Live.FollowedRoot, *target, 1.0f / static_cast<float>(stepsLeft));
		glm::vec3 position, scale;
		glm::quat rotation;
		if (!target || !DecomposeRagdollTransform(ScenePtr->GetWorldTransform(entity) * *target, position, rotation, scale))
		{
			// A kinematic body keeps its velocity: without a pose to follow it stops where it is.
			bodies.SetLinearAndAngularVelocity(root, JPH::Vec3::sZero(), JPH::Vec3::sZero());
			record.Live.FollowedRoot.reset();
			return;
		}
		record.Live.FollowedRoot = target;
		const JPH::RVec3 targetPosition = ToJolt(position);
		const JPH::Quat targetRotation = ToJolt(rotation).Normalized();
		JPH::RVec3 current;
		JPH::Quat currentRotation;
		bodies.GetPositionAndRotation(root, current, currentRotation);
		// A root sent there in one step would fling what hangs from it: the whole rig goes along instead, as it hung.
		if (IsRagdollTeleport(FromJolt(JPH::Vec3(targetPosition - current)), rotation, FromJolt(currentRotation), fixedStep))
		{
			const JPH::Quat turn = targetRotation * currentRotation.Conjugated();
			for (const JPH::BodyID part : record.Doll->GetBodyIDs())
			{
				JPH::RVec3 partPosition;
				JPH::Quat partRotation;
				bodies.GetPositionAndRotation(part, partPosition, partRotation);
				bodies.SetPositionAndRotation(part, targetPosition + turn * JPH::Vec3(partPosition - current), (turn * partRotation).Normalized(), JPH::EActivation::Activate);
				bodies.SetLinearAndAngularVelocity(part, turn * bodies.GetLinearVelocity(part), turn * bodies.GetAngularVelocity(part));
			}
		}
		bodies.MoveKinematic(root, targetPosition, targetRotation, fixedStep);
	}

	void PhysicsWorld::Impl::BalanceRagdoll(Entity entity, RagdollRecord& record, float fixedStep)
	{
		record.Live.Balanced = false;
		const JPH::Vec3 gravity = System->GetGravity();
		glm::vec3 unusedPosition, unusedScale;
		glm::quat animated;
		// Gravity can have stopped since the frame began.
		if (!record.Live.Balance || !record.Live.AnimatedRoot || gravity.IsNearZero() || !DecomposeRagdollTransform(ScenePtr->GetWorldTransform(entity) * *record.Live.AnimatedRoot, unusedPosition, animated, unusedScale))
			return;
		const RagdollRecord::BalanceDrive& drive = *record.Live.Balance;
		JPH::BodyInterface& bodies = System->GetBodyInterface();
		const JPH::Vec3 up = -gravity.Normalized();
		const JPH::BodyID root = record.Doll->GetBodyID(0);
		// Up in the animated root's frame, as the root body now carries it: upright when that is up again. No angles here
		// (nor below): they would need libm, and these forces must agree across platforms.
		const JPH::Vec3 carried = bodies.GetRotation(root) * (ToJolt(animated).Normalized().Conjugated() * up);
		if (carried.Dot(up) < drive.CosMaxTilt)
			return;
		const auto& parts = record.Doll->GetRagdollSettings()->mParts;
		JPH::RVec3 weighted = JPH::RVec3::sZero();
		JPH::Vec3 momentum = JPH::Vec3::sZero();
		float mass = 0;
		std::vector<float> heights;
		heights.reserve(parts.size());
		for (size_t i = 0; i < parts.size(); i++)
		{
			const JPH::BodyID part = record.Doll->GetBodyID(static_cast<int>(i));
			const float partMass = parts[i].mMassPropertiesOverride.mMass;
			weighted += bodies.GetCenterOfMassPosition(part) * partMass;
			momentum += bodies.GetLinearVelocity(part) * partMass;
			mass += partMass;
			heights.push_back(JPH::Vec3(bodies.GetPosition(part)).Dot(up));
		}
		const JPH::RVec3 center = weighted / mass;
		const std::optional<JPH::Vec3> support = FindRagdollSupport(entity.GetUUID(), record, center, up);
		if (!support)
			return;
		record.Live.Balanced = true;
		// A spring and damper on the tilt, integrated implicitly as Jolt's own springs are, so no frequency can overshoot a
		// step. Spinning about up is the feet's business.
		const JPH::Vec3 spin = bodies.GetAngularVelocity(root);
		const JPH::Vec3 tilting = spin - up * spin.Dot(up);
		const float implicit = drive.Strength / (1.0f + fixedStep * (drive.Damping + fixedStep * drive.Stiffness));
		const JPH::Vec3 acceleration = implicit * (drive.Stiffness * carried.Cross(up) - drive.Damping * tilting);
		// Standing: legs whose motors give way are pushed back to the animated pose's height. Only up, and at most
		// MaxStandAcceleration times gravity's, as legs push: a rig that sank far must not leap.
		const float rise = implicit * (drive.Stiffness * (drive.StandHeight - StandingHeight(*record.Doll, heights)) - drive.Damping * momentum.Dot(up) / mass);
		const float liftAcceleration = std::clamp(rise, 0.0f, MaxStandAcceleration * gravity.Length());
		// What the lift takes off the feet it also takes from their grip: the friction that weight would have given comes
		// back as a brake against what the rig stands on, or a rig standing on nothing but balance would skate away from any
		// push and stay behind on a moving floor.
		const JPH::Vec3 relative = momentum / mass - *support;
		const JPH::Vec3 drift = relative - up * relative.Dot(up);
		const float driftSpeed = drift.Length();
		const JPH::Vec3 brake = driftSpeed > 1e-4f ? drift * (-std::min(driftSpeed / fixedStep, PartFriction * liftAcceleration) / driftSpeed) : JPH::Vec3::sZero();
		const JPH::Vec3 lift = up * liftAcceleration + brake;
		// The whole rig turns at this rate about its center of mass, as one rigid body does: every part gets what that takes,
		// the turn's forces cancel out, and no joint has to transmit any of it.
		for (size_t i = 0; i < parts.size(); i++)
		{
			const JPH::BodyID part = record.Doll->GetBodyID(static_cast<int>(i));
			bodies.AddForce(part, parts[i].mMassPropertiesOverride.mMass * (lift + acceleration.Cross(JPH::Vec3(bodies.GetCenterOfMassPosition(part) - center))));
			bodies.AddTorque(part, bodies.GetInverseInertia(part).Inversed3x3().Multiply3x3(acceleration));
		}
	}

	bool PhysicsWorld::SetRagdollMotorStrength(Entity entity, float strength, float seconds)
	{
		auto& impl = *m_Impl;
		if (!impl.Owns(entity))
		{
			BS_CORE_ERROR("SetRagdollMotorStrength: entity is invalid or belongs to another scene");
			return false;
		}
		const auto* component = entity.TryGetComponent<RagdollComponent>();
		if (!component)
		{
			impl.MotorCallWarnings.Report(entity, { "SetRagdollMotorStrength: requires a Ragdoll component" });
			return false;
		}
		if (!IsValidFade(strength, seconds))
		{
			// Value-free, so a script computing a different bad value every frame logs once.
			impl.MotorCallWarnings.Report(entity, { "SetRagdollMotorStrength: the strength must be between 0 and 1 and the fade time finite and nonnegative" });
			return false;
		}
		impl.MotorCallWarnings.Report(entity, {});
		impl.MotorStrengthFor(entity.GetUUID(), *component).FadeTo(strength, seconds);
		return true;
	}

	std::optional<float> PhysicsWorld::GetRagdollMotorStrength(Entity entity) const
	{
		const auto* component = m_Impl->Owns(entity) ? entity.TryGetComponent<RagdollComponent>() : nullptr;
		if (!component)
			return std::nullopt;
		const auto it = m_Impl->RagdollStrength.find(entity.GetUUID());
		// FadingWeight's clamp, for a strength no call or frame has configured yet.
		return it != m_Impl->RagdollStrength.end() ? it->second.Weight.Get() : FadingWeight(component->MotorStrength).Get();
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

	bool PhysicsWorld::RagdollOwnsRoot(Entity entity) const
	{
		return HasRagdollPose(entity) && m_Impl->Ragdolls.at(entity.GetUUID()).OwnsRoot();
	}

	bool PhysicsWorld::IsRagdollBalanced(Entity entity) const
	{
		return IsRagdollActive(entity) && m_Impl->Ragdolls.at(entity.GetUUID()).Live.Balanced;
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
		if (record.Doll)
			pose.Powered = ForEachSwingTwist(*record.Doll, [](const JPH::SwingTwistConstraint& joint) { return joint.GetSwingMotorState() != JPH::EMotorState::Off || joint.GetTwistMotorState() != JPH::EMotorState::Off; });
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
