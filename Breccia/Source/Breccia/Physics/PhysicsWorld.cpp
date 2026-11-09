#include "Breccia/Physics/PhysicsWorld.h"

#include "Breccia/Core/Profiler.h"
#include "Breccia/Physics/ColliderShapes.h"
#include "Breccia/Physics/MeshShapeCache.h"
#include "Breccia/Physics/PhysicsWorldImpl.h"
#include "Breccia/Project/Project.h"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MassProperties.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Constraints/ContactConstraintManager.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace Breccia {

	using namespace PhysicsInternal;

	namespace {

		// Whether a change from `built` to `current` must rebuild the body (which ends its contacts) rather than apply in
		// place. Everything counts but what UpdateBody applies, so a new field rebuilds until it does. These fields are also
		// the RigidBody's ComponentInfo::AnimatableFields.
		bool BodyNeedsRebuild(const RigidBodyComponent& built, const RigidBodyComponent& current)
		{
			RigidBodyComponent structural = current;
			structural.Mass = built.Mass;
			structural.LinearDamping = built.LinearDamping;
			structural.AngularDamping = built.AngularDamping;
			structural.GravityFactor = built.GravityFactor;
			structural.Friction = built.Friction;
			structural.Restitution = built.Restitution;
			structural.FrictionCombine = built.FrictionCombine;
			structural.RestitutionCombine = built.RestitutionCombine;
			structural.Continuous = built.Continuous;
			structural.AllowSleeping = built.AllowSleeping;
			return structural != built;
		}

		// Whether the change reaches what touches the body.
		bool SurfaceChanged(const RigidBodyComponent& built, const RigidBodyComponent& current)
		{
			return built.Friction != current.Friction || built.Restitution != current.Restitution || built.FrictionCombine != current.FrictionCombine || built.RestitutionCombine != current.RestitutionCombine;
		}

		// A RigidBody's own settings in Jolt's terms. Their problems go to warnings, in a fixed order, so an update in place
		// logs what a rebuild would.
		struct BodySettings
		{
			uint32_t Layer = 0;
			ColliderMaterial Surface;
			float LinearDamping = 0.0f;
			float AngularDamping = 0.0f;
			JPH::EMotionQuality Quality = JPH::EMotionQuality::Discrete;
		};

		BodySettings CheckBodySettings(const RigidBodyComponent& rigidBody, const PhysicsLayers& layers, std::vector<std::string>& warnings)
		{
			BodySettings settings;
			if (const auto index = layers.Find(rigidBody.Layer))
				settings.Layer = *index;
			else
				warnings.emplace_back("unknown physics layer '" + rigidBody.Layer + "', using Default");
			settings.Surface = SanitizeSurface(rigidBody.Friction, rigidBody.Restitution, "", warnings);
			if (rigidBody.Type == RigidBodyType::Dynamic && rigidBody.Mass <= 0.0f)
				warnings.emplace_back("Mass must be positive, using the smallest mass");
			if (rigidBody.LinearDamping < 0.0f || rigidBody.AngularDamping < 0.0f)
				warnings.emplace_back("damping must not be negative, using 0");
			settings.LinearDamping = std::max(rigidBody.LinearDamping, 0.0f);
			settings.AngularDamping = std::max(rigidBody.AngularDamping, 0.0f);
			// Static bodies never sleep or wake and Jolt never puts a sensor to sleep, so turning sleeping off there has no effect.
			if (!rigidBody.AllowSleeping && (rigidBody.Type == RigidBodyType::Static || rigidBody.IsTrigger))
				warnings.emplace_back("AllowSleeping only affects dynamic and kinematic bodies that are not triggers");
			// Jolt only sweeps dynamic, non-sensor bodies.
			if (rigidBody.Continuous)
			{
				if (rigidBody.Type == RigidBodyType::Dynamic && !rigidBody.IsTrigger)
					settings.Quality = JPH::EMotionQuality::LinearCast;
				else
					warnings.emplace_back("Continuous only affects dynamic bodies that are not triggers");
			}
			return settings;
		}

		// The body's colliders as one shape, in the form its motion type and sensor flag need. Jolt cannot simulate a
		// triangle mesh on a dynamic body, so those always use the convex hull.
		ColliderShape BuildBodyShape(Entity entity, const glm::vec3& scale, const RigidBodyComponent& rigidBody, std::vector<std::string>& warnings)
		{
			return BuildColliderShape(entity, scale, 1.0f, rigidBody.Type == RigidBodyType::Dynamic ? "a dynamic body" : nullptr, rigidBody.IsTrigger, true, warnings);
		}

	}

	PhysicsWorld::Impl::BodyRecord* PhysicsWorld::Impl::FindBody(Entity entity) const
	{
		return Owns(entity) ? ScenePtr->GetRegistry().try_get<BodyRecord>(entity) : nullptr;
	}

	PhysicsWorld::Impl::BodyRecord* PhysicsWorld::Impl::FindBody(UUID uuid) const
	{
		return FindBody(ScenePtr->GetEntityByUUID(uuid));
	}

	PhysicsWorld::Impl::BodyRecord* PhysicsWorld::Impl::MutableBody(Entity entity, const char* call)
	{
		return Owns(entity) && !WarnIfCharacter(entity, call) ? FindBody(entity) : nullptr;
	}

	void PhysicsWorld::Impl::RemoveBody(UUID uuid)
	{
		Entity entity = ScenePtr->GetEntityByUUID(uuid);
		auto* record = FindBody(entity);
		if (!record)
			return;
		const bool trigger = record->Settings.IsTrigger;
		RemoveJointsOfBody(uuid);
		JPH::BodyInterface& bodies = System->GetBodyInterface();
		const JPH::BodyID removed = record->ID;
		BodyToEntity.erase(removed.GetIndexAndSequenceNumber());
		// Jolt reuses the index; the next body must not inherit these material modes.
		ContactListener.Materials[record->ID.GetIndex()] = {};
		bodies.RemoveBody(record->ID);
		bodies.DestroyBody(record->ID);
		ScenePtr->GetRegistry().remove<BodyRecord>(entity);
		EndContacts(uuid, std::span(&removed, 1), trigger);
	}

	bool PhysicsWorld::Impl::WritePose(Entity entity, const glm::vec3& position, const glm::quat& rotation)
	{
		Scene* scene = entity.GetScene();
		glm::vec3 unusedPosition;
		glm::quat unusedRotation;
		glm::vec3 scale;
		if (!Math::DecomposeTransform(scene->GetWorldTransform(entity), unusedPosition, unusedRotation, scale))
			return false;
		TransformComponent& transform = entity.GetTransform();
		const glm::vec3 localScale = transform.Scale;
		scene->SetWorldTransform(entity, Math::ComposeTransform(position, rotation, scale));
		// copysign, not glm::sign: a zero component of the decomposed scale still takes the magnitude.
		for (int axis = 0; axis < 3; axis++)
			transform.Scale[axis] = std::copysign(std::abs(localScale[axis]), transform.Scale[axis]);
		return true;
	}

	void PhysicsWorld::Impl::OnPhysicsComponentChanged(entt::registry& registry, entt::entity entity)
	{
		if (const auto* id = registry.try_get<IDComponent>(entity))
			DirtyEntities.Insert(id->ID);
	}

	void PhysicsWorld::Impl::OnMeshChanged(entt::registry& registry, entt::entity entity)
	{
		if (const auto* collider = registry.try_get<MeshColliderComponent>(entity); collider && collider->Mesh.empty())
			OnPhysicsComponentChanged(registry, entity);
	}

	void PhysicsWorld::Impl::OnMeshUpdated(entt::registry& registry, entt::entity entity)
	{
		const auto* id = registry.try_get<IDComponent>(entity);
		const auto& mesh = registry.get<MeshComponent>(entity);
		const std::pair borrowed(mesh.Mesh, mesh.MeshIndex);
		if (const auto* body = registry.try_get<BodyRecord>(entity); body && body->Colliders.BorrowedMesh == borrowed)
			return;
		if (auto it = id ? Characters.find(id->ID) : Characters.end(); it != Characters.end() && it->second.Colliders.BorrowedMesh == borrowed)
			return;
		OnMeshChanged(registry, entity);
	}

	void PhysicsWorld::Impl::WireSignals(entt::registry& registry, bool connect)
	{
		constexpr auto Changed = &Impl::OnPhysicsComponentChanged;
		Wire<RigidBodyComponent, Changed, Changed, Changed>(registry, connect);
		Wire<BoxColliderComponent, Changed, Changed, Changed>(registry, connect);
		Wire<SphereColliderComponent, Changed, Changed, Changed>(registry, connect);
		Wire<CapsuleColliderComponent, Changed, Changed, Changed>(registry, connect);
		Wire<MeshColliderComponent, Changed, Changed, Changed>(registry, connect);
		Wire<MeshComponent, &Impl::OnMeshChanged, &Impl::OnMeshUpdated, &Impl::OnMeshChanged>(registry, connect);
		// Adding or removing a controller swaps a body for a character, so it rebuilds like adding or removing a collider.
		Wire<CharacterControllerComponent, Changed, &Impl::OnCharacterChanged, Changed>(registry, connect);
		Wire<JointComponent, &Impl::OnJointChanged, &Impl::OnJointChanged, &Impl::OnJointDestroyed>(registry, connect);
		if (connect)
		{
			registry.on_destroy<RagdollComponent>().connect<&Impl::OnRagdollDestroyed>(*this);
			registry.on_destroy<AnimatorComponent>().connect<&Impl::OnRagdollDestroyed>(*this);
		}
		else
		{
			registry.on_destroy<RagdollComponent>().disconnect<&Impl::OnRagdollDestroyed>(*this);
			registry.on_destroy<AnimatorComponent>().disconnect<&Impl::OnRagdollDestroyed>(*this);
		}
	}

	namespace {

		PhysicsCapacity s_DefaultCapacity;

	}

	const PhysicsCapacity& PhysicsWorld::GetDefaultCapacity()
	{
		return s_DefaultCapacity;
	}

	void PhysicsWorld::SetDefaultCapacity(const PhysicsCapacity& capacity)
	{
		s_DefaultCapacity = capacity;
	}

	void PhysicsWorld::Impl::ReportUpdateErrors(JPH::EPhysicsUpdateError errors)
	{
		// Once per kind: a scene over a limit stays over it, and this runs every fixed step.
		uint32_t unseen = static_cast<uint32_t>(errors) & ~ReportedUpdateErrors;
		ReportedUpdateErrors |= unseen;
		const auto report = [&unseen](JPH::EPhysicsUpdateError error, std::string_view what, std::string_view limit, uint32_t value) {
			if (unseen & static_cast<uint32_t>(error))
				BR_CORE_ERROR("Physics: too many {} touching at once ({} is {}); contacts were dropped, so bodies pass through what they touch until fewer of them touch", what, limit, value);
			unseen &= ~static_cast<uint32_t>(error);
		};
		report(JPH::EPhysicsUpdateError::BodyPairCacheFull, "body pairs", "MaxBodyPairs", Capacity.MaxBodyPairs);
		report(JPH::EPhysicsUpdateError::ManifoldCacheFull, "contact manifolds", "MaxContactConstraints", Capacity.MaxContactConstraints);
		report(JPH::EPhysicsUpdateError::ContactConstraintsFull, "contact constraints", "MaxContactConstraints", Capacity.MaxContactConstraints);
		if (unseen != 0)
			BR_CORE_ERROR("Physics: the update failed in a way Breccia does not know (Jolt error bits {:#x})", unseen);
	}

	PhysicsWorld::PhysicsWorld(Scene* scene, PhysicsCapacity capacity)
		: m_Impl(CreateScope<Impl>(scene))
	{
		AcquireJolt();

		// What Jolt will keep, so the rest is sized from that and messages say it. It asserts on more, and raises less.
		constexpr uint32_t SmallestTable = 4;
		capacity.MaxBodies = std::min(capacity.MaxBodies, static_cast<uint32_t>(JPH::PhysicsSystem::cMaxBodiesLimit));
		capacity.MaxBodyPairs = std::clamp(capacity.MaxBodyPairs, SmallestTable, static_cast<uint32_t>(JPH::PhysicsSystem::cMaxBodyPairsLimit));
		capacity.MaxContactConstraints = std::clamp(capacity.MaxContactConstraints, SmallestTable, static_cast<uint32_t>(JPH::PhysicsSystem::cMaxContactConstraintsLimit));

		constexpr uint32_t NumBodyMutexes = 0; // Jolt default
		// On top of what the contact constraints take, for everything else a step allocates.
		constexpr uint64_t TempHeadroomBytes = uint64_t{ 16 } * 1024 * 1024;
		const uint64_t tempBytes = capacity.TempMemoryBytes != 0 ? capacity.TempMemoryBytes : capacity.MaxContactConstraints * uint64_t{ JPH::ContactConstraintManager::cMaxConstraintSize } + TempHeadroomBytes;

		m_Impl->Capacity = capacity;
		m_Impl->TempAllocator = CreateScope<JPH::TempAllocatorImplWithMallocFallback>(static_cast<JPH::uint>(std::min<uint64_t>(tempBytes, std::numeric_limits<JPH::uint>::max())));
		m_Impl->JobSystem = &PhysicsJobs::Get();

		m_Impl->Layers = scene->ResolvePhysicsLayers();
		m_Impl->ObjectLayerPairFilter.SetLayers(m_Impl->Layers);

		m_Impl->System = CreateScope<JPH::PhysicsSystem>();
		m_Impl->System->Init(capacity.MaxBodies, NumBodyMutexes, capacity.MaxBodyPairs, capacity.MaxContactConstraints,
							 m_Impl->BroadPhaseLayerInterface, m_Impl->ObjectVsBroadPhaseLayerFilter, m_Impl->ObjectLayerPairFilter);
		m_Impl->ContactListener.Materials.resize(capacity.MaxBodies);
		m_Impl->ContactListener.MinVelocityForRestitution = m_Impl->System->GetPhysicsSettings().mMinVelocityForRestitution;
		m_Impl->System->SetContactListener(&m_Impl->ContactListener);
		// Bodies joined by a joint do not collide with each other unless the joint asks for it.
		m_Impl->System->SetSimShapeFilter(&m_Impl->BodyPairs);
		SetGravity(scene->GetPhysicsSettings().Gravity);

		entt::registry& registry = scene->GetRegistry();
		m_Impl->WireSignals(registry, true);
	}

	PhysicsWorld::~PhysicsWorld()
	{
		entt::registry& registry = m_Impl->ScenePtr->GetRegistry();
		m_Impl->WireSignals(registry, false);

		m_Impl->ShuttingDown = true;
		while (!m_Impl->Ragdolls.empty())
			m_Impl->RemoveRagdoll(m_Impl->Ragdolls.begin()->first);

		for (auto& [uuid, joint] : m_Impl->Joints)
			m_Impl->System->RemoveConstraint(joint.Constraint);
		m_Impl->Joints.clear();
		// Characters remove their inner bodies, so they go while the system exists.
		m_Impl->CharacterCollision.mCharacters.clear();
		m_Impl->Characters.clear();
		JPH::BodyInterface& bodies = m_Impl->System->GetBodyInterface();
		for (auto [handle, record] : registry.view<Impl::BodyRecord>().each())
		{
			bodies.RemoveBody(record.ID);
			bodies.DestroyBody(record.ID);
		}
		registry.clear<Impl::BodyRecord>();
		m_Impl->System.reset();
		m_Impl->JobSystem = nullptr;
		m_Impl->TempAllocator.reset();
		m_Impl.reset();

		ReleaseJolt();
	}

	void PhysicsWorld::Start()
	{
		Impl& impl = *m_Impl;
		auto view = impl.ScenePtr->GetAllEntitiesWith<RigidBodyComponent>();
		impl.Starting = true;
		for (entt::entity handle : view)
			impl.RecreateBody({ handle, impl.ScenePtr });
		for (entt::entity handle : impl.ScenePtr->GetAllEntitiesWith<CharacterControllerComponent>())
			impl.RecreateCharacter({ handle, impl.ScenePtr });
		impl.Starting = false;
		impl.DirtyEntities.Clear();
		impl.DirtyCharacters.Clear();
		// Joints need both of their bodies, so they are built after every body exists.
		for (entt::entity handle : impl.ScenePtr->GetAllEntitiesWith<JointComponent>())
			impl.DirtyJoints.insert(Entity(handle, impl.ScenePtr).GetUUID());
		impl.RebuildDirtyJoints();
		if (impl.ScenePtr->GetAnimationSystem())
			impl.SyncRagdolls(*this, 0);
		impl.System->OptimizeBroadPhase();
	}

	void PhysicsWorld::RecreateBody(Entity entity)
	{
		m_Impl->RecreateBody(entity);
	}

	void PhysicsWorld::Impl::RecreateBody(Entity entity)
	{
		// UUIDs can match across scene copies, but EnTT handles belong to only one registry.
		if (!Owns(entity))
		{
			BR_CORE_WARN("Physics: cannot rebuild {} entity '{}'", entity ? "foreign" : "invalid", entity ? entity.GetName() : "<invalid>");
			return;
		}
		JPH::BodyInterface& bodies = System->GetBodyInterface();
		JPH::Vec3 previousLinearVelocity = JPH::Vec3::sZero();
		JPH::Vec3 previousAngularVelocity = JPH::Vec3::sZero();

		if (auto* existing = FindBody(entity))
		{
			// Rebuilding (e.g. after a new Type or Layer) keeps the body moving.
			previousLinearVelocity = bodies.GetLinearVelocity(existing->ID);
			previousAngularVelocity = bodies.GetAngularVelocity(existing->ID);
			RemoveBody(entity.GetUUID());
		}

		// Removing a body dispatches contact exits, whose scripts can destroy the entity.
		// A character controller replaces the body (RecreateCharacter reports the ignored RigidBody).
		if (!entity || !entity.HasComponent<RigidBodyComponent>() || entity.HasComponent<CharacterControllerComponent>() || RagdollReplacesBody(entity.GetUUID()))
			return;

		const RigidBodyComponent rigidBody = EffectiveRigidBody(entity, entity.GetComponent<RigidBodyComponent>());
		glm::vec3 position;
		glm::quat rotation;
		glm::vec3 scale;
		if (!Math::DecomposeTransform(ScenePtr->GetWorldTransform(entity), position, rotation, scale))
		{
			BR_CORE_WARN("Physics: entity '{}' has a degenerate transform; no body created", entity.GetName());
			Unbuilt.insert(entity.GetUUID());
			return;
		}
		Unbuilt.erase(entity.GetUUID());

		JPH::EMotionType motionType = JPH::EMotionType::Static;
		if (rigidBody.Type == RigidBodyType::Dynamic)
			motionType = JPH::EMotionType::Dynamic;
		else if (rigidBody.Type == RigidBodyType::Kinematic)
			motionType = JPH::EMotionType::Kinematic;

		// Problems with the body's settings, joined with "; ". Logged when they differ from the last ones
		// logged for this entity, so a script that sets the component every frame does not flood the log.
		std::vector<std::string> warnings;
		auto reportWarnings = [&]() { BodyWarnings.Report(entity, warnings); };

		const ColliderShape colliders = BuildBodyShape(entity, scale, rigidBody, warnings);
		if (!colliders.Shape)
		{
			warnings.emplace_back("no body created");
			reportWarnings();
			return;
		}
		const bool hasTriangleMesh = colliders.HasTriangleMesh;

		std::vector<std::string> shapeWarnings = warnings;
		const BodySettings checked = CheckBodySettings(rigidBody, Layers, warnings);
		const bool moving = motionType != JPH::EMotionType::Static;
		JPH::BodyCreationSettings settings(colliders.Shape, ToJolt(position), ToJolt(rotation), motionType, MakeObjectLayer(moving, checked.Layer));
		settings.mUserData = static_cast<uint64_t>(entity.GetUUID());
		settings.mFriction = checked.Surface.Friction;
		settings.mRestitution = checked.Surface.Restitution;
		settings.mLinearDamping = checked.LinearDamping;
		settings.mAngularDamping = checked.AngularDamping;
		settings.mGravityFactor = rigidBody.GravityFactor;
		settings.mIsSensor = rigidBody.IsTrigger;
		settings.mAllowSleeping = rigidBody.AllowSleeping;
		settings.mMotionQuality = checked.Quality;
		// Kinematic triggers must also see static bodies; kinematic bodies are otherwise skipped vs static.
		settings.mCollideKinematicVsNonDynamic = rigidBody.IsTrigger;
		if (rigidBody.FixedRotation && motionType == JPH::EMotionType::Dynamic)
			settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
		reportWarnings();
		// A triangle mesh has no volume, so Jolt cannot derive the mass a kinematic body still needs; a solid
		// box of the shape's bounds stands in (kinematic bodies are not moved by forces, only its presence counts).
		if (motionType == JPH::EMotionType::Kinematic && hasTriangleMesh)
		{
			const JPH::AABox bounds = colliders.Shape->GetLocalBounds();
			settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
			settings.mMassPropertiesOverride.SetMassAndInertiaOfSolidBox(JPH::Vec3::sMax(bounds.GetSize(), JPH::Vec3::sReplicate(MinExtent)), 1.0f);
			settings.mMassPropertiesOverride.ScaleToMass(std::max(rigidBody.Mass, 0.001f));
		}
		if (motionType == JPH::EMotionType::Dynamic)
		{
			settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
			settings.mMassPropertiesOverride.mMass = std::max(rigidBody.Mass, 0.001f);
		}

		const JPH::EActivation activation = moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
		const JPH::BodyID bodyID = bodies.CreateAndAddBody(settings, activation);
		if (bodyID.IsInvalid())
		{
			BR_CORE_ERROR("Physics: body limit reached; no body created for '{}'", entity.GetName());
			return;
		}

		BodyMaterial& material = ContactListener.Materials[bodyID.GetIndex()];
		material = { .FrictionCombine = rigidBody.FrictionCombine, .RestitutionCombine = rigidBody.RestitutionCombine };
		if (std::ranges::any_of(colliders.Materials, &ColliderMaterial::Override))
			material.Colliders = CreateScope<const ColliderMaterials>(colliders.Materials);

		Impl::BodyRecord record;
		record.ID = bodyID;
		record.LastPosition = position;
		record.LastRotation = rotation;
		record.BuiltScale = scale;
		record.Settings = rigidBody;
		record.Colliders = ColliderSettings::Capture(entity);
		record.ShapeWarnings = std::move(shapeWarnings);
		ScenePtr->GetRegistry().emplace<BodyRecord>(entity, std::move(record));
		BodyToEntity[bodyID.GetIndexAndSequenceNumber()] = entity.GetUUID();
		if (motionType == JPH::EMotionType::Dynamic)
		{
			bodies.SetLinearVelocity(bodyID, previousLinearVelocity);
			bodies.SetAngularVelocity(bodyID, previousAngularVelocity);
		}
		// Joints that could not be built without this body (or were removed with its old one) can be now.
		if (!Starting)
			MarkJointsDirty(entity.GetUUID());
	}

	void PhysicsWorld::Impl::ApplyScaleChanges()
	{
		// Relative, so tiny and huge scales are treated alike; decomposing the same transform again only
		// differs in the last bits, far below this.
		auto rescaled = [](const glm::vec3& scale, const glm::vec3& built) {
			return glm::any(glm::greaterThan(glm::abs(scale - built), glm::abs(built) * 1e-4f + glm::vec3(1e-7f)));
		};
		// Collected in registry order first: a fallback rebuild can end contacts and so run scripts.
		std::vector<std::pair<Entity, glm::vec3>> resizedBodies;
		std::vector<std::pair<Entity, glm::vec3>> resizedCharacters;
		std::vector<Entity> recovered;
		auto collect = [&](entt::entity handle, const auto* record, auto& resized) {
			Entity entity(handle, ScenePtr);
			glm::vec3 position;
			glm::quat rotation;
			glm::vec3 scale;
			// A degenerate transform keeps the old shape; no new one could be built from it.
			if (!Math::DecomposeTransform(ScenePtr->GetWorldTransform(entity), position, rotation, scale))
				return;
			if (record)
			{
				if (rescaled(scale, record->BuiltScale))
					resized.emplace_back(entity, scale);
			}
			else if (Unbuilt.contains(entity.GetUUID()))
			{
				recovered.push_back(entity);
			}
		};
		for (entt::entity handle : ScenePtr->GetAllEntitiesWith<RigidBodyComponent>())
			collect(handle, ScenePtr->GetRegistry().try_get<BodyRecord>(handle), resizedBodies);
		for (entt::entity handle : ScenePtr->GetAllEntitiesWith<CharacterControllerComponent>())
		{
			auto it = Characters.find(Entity(handle, ScenePtr).GetUUID());
			collect(handle, it == Characters.end() ? nullptr : &it->second, resizedCharacters);
		}

		for (auto& [entity, scale] : resizedBodies)
			ResizeBody(entity, scale);
		for (auto& [entity, scale] : resizedCharacters)
			ResizeCharacter(entity, scale);
		for (Entity entity : recovered)
		{
			if (entity)
			{
				RecreateBody(entity);
				RecreateCharacter(entity);
			}
		}
	}

	void PhysicsWorld::Impl::ResizeBody(Entity entity, const glm::vec3& scale)
	{
		auto* found = FindBody(entity.GetUUID());
		if (found == nullptr)
			return;
		Impl::BodyRecord& record = *found;
		std::vector<std::string> warnings;
		const ColliderShape colliders = BuildBodyShape(entity, scale, record.Settings, warnings);
		ResizeWarnings.Report(entity, warnings);
		if (!colliders.Shape)
		{
			RecreateBody(entity);
			return;
		}

		// Joint anchors are scaled with the body (see CreateJoint), so its joints are rebuilt from the current
		// poses before the next step, as after any rebuild. Constraints must go before their body's shape changes.
		RemoveJointsOfBody(entity.GetUUID());
		if (SwapBodyShape(entity, record, colliders))
			record.BuiltScale = scale;
		else
			RecreateBody(entity);
	}

	bool PhysicsWorld::Impl::SwapBodyShape(Entity entity, BodyRecord& record, const ColliderShape& colliders)
	{
		System->GetBodyInterface().SetShape(record.ID, colliders.Shape, false, JPH::EActivation::Activate);
		// Shape cooking can omit a failed child, so replace its material table as well.
		BodyMaterial& material = ContactListener.Materials[record.ID.GetIndex()];
		material.Colliders.reset();
		if (std::ranges::any_of(colliders.Materials, &ColliderMaterial::Override))
			material.Colliders = CreateScope<const ColliderMaterials>(colliders.Materials);
		return record.Settings.Type != RigidBodyType::Dynamic || ApplyBodyMass(entity, record, entity.GetComponent<RigidBodyComponent>().Mass);
	}

	bool PhysicsWorld::Impl::ApplyBodyMass(Entity entity, const BodyRecord& record, float mass)
	{
		// As in RecreateBody: the shape's inertia at the component's mass, not at Jolt's density.
		JPH::BodyLockWrite lock(System->GetBodyLockInterface(), record.ID);
		if (!lock.Succeeded())
		{
			BR_CORE_ERROR("Physics: cannot lock the body of '{}' to change its mass; rebuilding it", entity.GetName());
			return false;
		}
		JPH::Body& body = lock.GetBody();
		JPH::MotionProperties* motion = body.GetMotionProperties();
		JPH::MassProperties properties = body.GetShape()->GetMassProperties();
		properties.ScaleToMass(std::max(mass, 0.001f));
		motion->SetMassProperties(motion->GetAllowedDOFs(), properties);
		return true;
	}

	bool PhysicsWorld::Impl::UpdateInPlace(Entity entity)
	{
		if (RagdollReplacesBody(entity.GetUUID()))
			return false;
		const ColliderSettings colliders = ColliderSettings::Capture(entity);
		if (auto* record = FindBody(entity))
			return UpdateBody(entity, *record, colliders);
		const auto character = Characters.find(entity.GetUUID());
		return character != Characters.end() && UpdateCharacter(entity, character->second, colliders);
	}

	bool PhysicsWorld::Impl::ReplaceBodyColliders(Entity entity, BodyRecord& record, const ColliderSettings& colliders)
	{
		JPH::BodyInterface& bodies = System->GetBodyInterface();
		std::vector<std::string> warnings;
		const ColliderShape shape = BuildBodyShape(entity, record.BuiltScale, record.Settings, warnings);
		if (!shape.Shape)
			return false;
		const JPH::Vec3 previousCenter = bodies.GetShape(record.ID)->GetCenterOfMass();
		if (!SwapBodyShape(entity, record, shape))
			return false;
		// The anchors do not depend on the colliders' size, so unlike a rescale this leaves joints standing. Rebuilding
		// would reset their rest poses every frame of an animated collider, and with them every limit.
		if (const JPH::Vec3 moved = shape.Shape->GetCenterOfMass() - previousCenter; moved != JPH::Vec3::sZero())
			for (const auto& [holder, joint] : Joints)
				if (joint.Touches(entity.GetUUID()))
					joint.Constraint->NotifyShapeChanged(record.ID, moved);
		record.Colliders = colliders;
		record.ShapeWarnings = std::move(warnings);
		return true;
	}

	bool PhysicsWorld::Impl::UpdateBody(Entity entity, BodyRecord& record, const ColliderSettings& colliders)
	{
		const auto* component = entity.TryGetComponent<RigidBodyComponent>();
		if (!component)
			return false;
		const RigidBodyComponent effective = EffectiveRigidBody(entity, *component);
		const RigidBodyComponent* rigidBody = &effective;
		if (entity.HasComponent<CharacterControllerComponent>() || BodyNeedsRebuild(record.Settings, *rigidBody) || !colliders.SameChildren(record.Colliders))
			return false;
		const bool reshaped = colliders != record.Colliders;
		// Rewriting the same values must not wake the body or its neighbours.
		if (!reshaped && *rigidBody == record.Settings)
			return true;
		JPH::BodyInterface& bodies = System->GetBodyInterface();
		JPH::AABox bounds = bodies.GetTransformedShape(record.ID).GetWorldSpaceBounds();
		const bool touchChanged = reshaped || SurfaceChanged(record.Settings, *rigidBody);
		if (reshaped)
		{
			if (!ReplaceBodyColliders(entity, record, colliders))
				return false;
			bounds.Encapsulate(bodies.GetTransformedShape(record.ID).GetWorldSpaceBounds());
		}
		std::vector<std::string> warnings = record.ShapeWarnings;
		const BodySettings checked = CheckBodySettings(*rigidBody, Layers, warnings);
		bodies.SetFriction(record.ID, checked.Surface.Friction);
		bodies.SetRestitution(record.ID, checked.Surface.Restitution);
		BodyMaterial& material = ContactListener.Materials[record.ID.GetIndex()];
		material.FrictionCombine = rigidBody->FrictionCombine;
		material.RestitutionCombine = rigidBody->RestitutionCombine;
		if (record.Settings.Type != RigidBodyType::Static)
		{
			bodies.SetGravityFactor(record.ID, rigidBody->GravityFactor);
			bodies.SetMotionQuality(record.ID, checked.Quality);
			{
				JPH::BodyLockWrite lock(System->GetBodyLockInterface(), record.ID);
				if (!lock.Succeeded())
				{
					BR_CORE_ERROR("Physics: cannot lock the body of '{}' to update it; rebuilding it", entity.GetName());
					return false;
				}
				JPH::Body& body = lock.GetBody();
				JPH::MotionProperties* motion = body.GetMotionProperties();
				motion->SetLinearDamping(checked.LinearDamping);
				motion->SetAngularDamping(checked.AngularDamping);
				body.SetAllowSleeping(rigidBody->AllowSleeping);
			}
			// SwapBodyShape has applied the current mass with a new shape.
			const bool newMass = record.Settings.Type == RigidBodyType::Dynamic && !reshaped && rigidBody->Mass != record.Settings.Mass;
			if (newMass && !ApplyBodyMass(entity, record, rigidBody->Mass))
				return false;
			// Activating a kinematic body wakes what it touches, which only a change that reaches them should.
			if (touchChanged || record.Settings.Type == RigidBodyType::Dynamic)
				bodies.ActivateBody(record.ID);
		}
		// What rests on a static or kinematic body sleeps on, not linked to it, while the ground changes under it: a
		// floor turning to ice must set its crates sliding. A body that moves wakes what it touches by itself.
		if (touchChanged && record.Settings.Type != RigidBodyType::Dynamic)
		{
			constexpr float TouchMargin = 0.05f;
			bounds.ExpandBy(JPH::Vec3::sReplicate(TouchMargin));
			bodies.ActivateBodiesInAABox(bounds, JPH::BroadPhaseLayerFilter(), JPH::ObjectLayerFilter());
		}
		record.Settings = *rigidBody;
		BodyWarnings.Report(entity, warnings);
		return true;
	}

	RigidBodyComponent PhysicsWorld::Impl::EffectiveRigidBody(Entity entity, const RigidBodyComponent& component) const
	{
		RigidBodyComponent effective = component;
		if (effective.Type == RigidBodyType::Dynamic && ExternallyDriven.contains(entity.GetUUID()))
			effective.Type = RigidBodyType::Kinematic;
		return effective;
	}

	void PhysicsWorld::SetDrivenExternally(Entity entity, bool driven)
	{
		if (!m_Impl->Owns(entity))
			return;
		const UUID uuid = entity.GetUUID();
		const bool changed = driven ? m_Impl->ExternallyDriven.insert(uuid).second : m_Impl->ExternallyDriven.erase(uuid) > 0;
		// The next update compares the body with what it should be built from now, and rebuilds it with the new type.
		if (changed && entity.HasComponent<RigidBodyComponent>())
			m_Impl->DirtyEntities.Insert(uuid);
	}

	bool PhysicsWorld::IsDrivenExternally(Entity entity) const
	{
		return m_Impl->Owns(entity) && m_Impl->ExternallyDriven.contains(entity.GetUUID());
	}

	void PhysicsWorld::OnEntityDestroyed(Entity entity)
	{
		if (!m_Impl->Owns(entity))
			return;
		const bool blocked = m_Impl->UnavailableRagdolls.insert(entity.GetUUID()).second;
		m_Impl->RemoveRagdoll(entity.GetUUID());
		m_Impl->RagdollFailures.erase(entity.GetUUID());
		m_Impl->RagdollWarnings.Forget(entity.GetUUID());
		m_Impl->RemoveBody(entity.GetUUID());
		m_Impl->RemoveCharacter(entity.GetUUID());
		m_Impl->RemoveJoint(entity.GetUUID());
		m_Impl->CharacterWarnings.Forget(entity.GetUUID());
		m_Impl->DirtyJoints.erase(entity.GetUUID());
		m_Impl->JointWarnings.Forget(entity.GetUUID());
		m_Impl->BodyWarnings.Forget(entity.GetUUID());
		m_Impl->ResizeWarnings.Forget(entity.GetUUID());
		m_Impl->Unbuilt.erase(entity.GetUUID());
		m_Impl->ExternallyDriven.erase(entity.GetUUID());
		if (blocked)
			m_Impl->UnavailableRagdolls.erase(entity.GetUUID());
	}

	bool PhysicsWorld::HasBody(Entity entity) const
	{
		return m_Impl->FindBody(entity) != nullptr;
	}

	void PhysicsWorld::OnSceneLoaded()
	{
		m_Impl->ApplyPendingChanges();
		if (m_Impl->ScenePtr->GetAnimationSystem())
			m_Impl->SyncRagdolls(*this, 0);
		// Like Start: many bodies were just added.
		m_Impl->System->OptimizeBroadPhase();
	}

	void PhysicsWorld::Impl::ApplyPendingChanges()
	{
		// Update or rebuild bodies whose physics components changed since the last step, then resize rescaled ones.
		for (UUID uuid : DirtyEntities.Take())
		{
			Entity entity = ScenePtr->GetEntityByUUID(uuid);
			if (entity)
			{
				if (UpdateInPlace(entity))
					continue;
				RecreateBody(entity);
				RecreateCharacter(entity);
			}
			else
			{
				RemoveCharacter(uuid);
				// Scene destruction must notify us before removing the runtime BodyRecord.
				BR_CORE_ASSERT(std::ranges::none_of(BodyToEntity, [uuid](const auto& entry) { return entry.second == uuid; }), "Entity destroyed without physics notification");
			}
		}
		for (UUID uuid : DirtyCharacters.Take())
		{
			if (Entity entity = ScenePtr->GetEntityByUUID(uuid))
				ApplyCharacterSettings(entity);
		}
		ApplyScaleChanges();
		RebuildDirtyJoints();
	}

	void PhysicsWorld::Step(Timestep ts)
	{
		BR_PROFILE_SCOPE("Physics");
		Impl& impl = *m_Impl;
		JPH::BodyInterface& bodies = impl.System->GetBodyInterface();
		const PhysicsSettings& settings = impl.ScenePtr->GetPhysicsSettings();
		const float fixedStep = impl.ScenePtr->GetFixedTimestep();

		impl.ApplyPendingChanges();
		impl.SyncRagdolls(*this, ts.GetSeconds());
		// Restore bodies after ragdoll removal or recovery for this frame's steps, without scanning every body twice
		// on frames where ragdoll control changed nothing.
		if (!impl.DirtyEntities.Empty())
			impl.ApplyPendingChanges();

		m_Accumulator += ts.GetSeconds();
		// A frame that queued no root motion while earlier displacement waits for a step owns its part of that step for
		// gameplay movement (share 0), whatever kept the animation from queuing (a fade that ended, a stopped clip).
		if (const float seconds = ts.GetSeconds(); std::isfinite(seconds) && seconds > 0)
			for (auto& [id, request] : impl.RootMotion)
				if (!request.Active && request.HasQueued())
					request.Time += seconds;
		// Preserve animation displacement on frames below the fixed timestep, and distribute it over this
		// frame's substeps. Finishing or removing an Animator must not drop its last queued displacement.
		uint32_t plannedSteps = 0;
		float remaining = m_Accumulator;
		while (remaining >= fixedStep && plannedSteps < settings.MaxStepsPerFrame)
		{
			remaining -= fixedStep;
			plannedSteps++;
		}
		for (auto& [id, character] : impl.Characters)
		{
			character.RootMotion.reset();
			if (auto it = impl.RootMotion.find(id); plannedSteps > 0 && it != impl.RootMotion.end())
			{
				auto& request = it->second;
				if (request.Active || request.HasQueued())
					character.RootMotion = Impl::CharacterRecord::RootMotionDrive{ .Velocity = request.Pending / (fixedStep * static_cast<float>(plannedSteps)), .Share = request.Time > 0 ? request.ShareTime / request.Time : 1.0f };
				request.ClearQueued();
			}
		}
		// Active is a one-frame override, including zero-motion frames. Pending displacement survives
		// frames without a fixed step, but an invalid/skipped animation update cannot leave Active latched.
		for (auto it = impl.RootMotion.begin(); it != impl.RootMotion.end();)
		{
			auto& request = it->second;
			Entity entity = impl.ScenePtr->GetEntityByUUID(it->first);
			const bool built = impl.Characters.contains(it->first);
			if (!entity || !entity.HasComponent<CharacterControllerComponent>())
			{
				it = impl.RootMotion.erase(it);
				continue;
			}
			if (!built && (request.Active || request.HasQueued()))
			{
				if (!request.UnbuiltReported)
					BR_CORE_ERROR("Physics: character '{}' could not be built; discarding queued root motion translation", entity.GetName());
				request.UnbuiltReported = true;
				request.ClearQueued();
			}
			else if (built)
				request.UnbuiltReported = false;
			request.Active = false;
			// Reported problems keep their requests, so a caller repeating them logs once.
			if (built && !request.HasQueued() && !request.InvalidReported && !request.InvalidShareReported)
				it = impl.RootMotion.erase(it);
			else
				++it;
		}
		uint32_t steps = 0;
		while (m_Accumulator >= fixedStep && steps < settings.MaxStepsPerFrame)
		{
			auto bodyView = impl.BodyView();
			// Push entity-side transform changes into the simulation, in registry order: activating bodies
			// orders Jolt's active list, so a hash order would differ between standard libraries.
			for (auto [handle, rigidBody, record] : bodyView.each())
			{
				Entity entity(handle, impl.ScenePtr);

				glm::vec3 position;
				glm::quat rotation;
				glm::vec3 scale;
				if (!Math::DecomposeTransform(impl.ScenePtr->GetWorldTransform(entity), position, rotation, scale))
					continue;

				const bool moved = glm::any(glm::greaterThan(glm::abs(position - record.LastPosition), glm::vec3(1e-5f))) || glm::abs(glm::dot(rotation, record.LastRotation)) < 1.0f - 1e-6f;
				if (record.Settings.Type == RigidBodyType::Kinematic)
				{
					bodies.MoveKinematic(record.ID, ToJolt(position), ToJolt(rotation), fixedStep);
				}
				else if (moved)
				{
					const JPH::EActivation activation = record.Settings.Type == RigidBodyType::Dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
					bodies.SetPositionAndRotation(record.ID, ToJolt(position), ToJolt(rotation), activation);
				}
				record.LastPosition = position;
				record.LastRotation = rotation;
			}

			// Characters move before the bodies step, as Jolt expects.
			impl.UpdateCharacters(fixedStep);
			impl.StepRagdolls(fixedStep, std::max(plannedSteps - steps, 1u));

			impl.ReportUpdateErrors(impl.System->Update(fixedStep, 1, impl.TempAllocator.get(), impl.JobSystem));
			impl.ValidateRagdollOutput();
			m_Accumulator -= fixedStep;
			m_StepCount++;
			steps++;

			// Write simulated poses of dynamic bodies back to their entities, parents before children: a
			// child's local transform is derived from its parent's world transform, which must already be
			// this step's.
			std::vector<std::pair<uint32_t, Entity>> movedBodies;
			for (auto [handle, rigidBody, record] : bodyView.each())
			{
				Entity entity(handle, impl.ScenePtr);
				if (record.Settings.Type == RigidBodyType::Dynamic)
					movedBodies.emplace_back(impl.ScenePtr->GetDepth(entity), entity);
			}
			std::ranges::stable_sort(movedBodies, {}, &std::pair<uint32_t, Entity>::first);
			for (auto& [depth, entity] : movedBodies)
			{
				Impl::BodyRecord& record = entity.GetComponent<Impl::BodyRecord>();

				JPH::RVec3 joltPosition;
				JPH::Quat joltRotation;
				bodies.GetPositionAndRotation(record.ID, joltPosition, joltRotation);
				const glm::vec3 position = FromJolt(joltPosition);
				const glm::quat rotation = glm::normalize(FromJolt(joltRotation));

				// While the transform is degenerate the entity keeps its last written pose. LastPosition stays with
				// it, so once the transform is valid again the body is not teleported back but written out.
				if (!Impl::WritePose(entity, position, rotation))
				{
					if (!std::exchange(record.Degenerate, true))
						BR_CORE_WARN("Physics: entity '{}' has a degenerate transform (e.g. a zero scale); its body keeps its shape and its pose is not written back until the transform is valid", entity.GetName());
					continue;
				}
				record.Degenerate = false;
				record.LastPosition = position;
				record.LastRotation = rotation;
			}

			impl.CheckBrokenJoints(fixedStep);
			impl.DispatchContacts(fixedStep);
		}

		// Never let a long stall build an unbounded backlog of steps.
		if (steps == settings.MaxStepsPerFrame)
			m_Accumulator = std::min(m_Accumulator, fixedStep);
	}

	void PhysicsWorld::AddForce(Entity entity, const glm::vec3& force)
	{
		auto* body = m_Impl->MutableBody(entity, "AddForce");
		if (body != nullptr)
			m_Impl->System->GetBodyInterface().AddForce(body->ID, ToJolt(force));
	}

	void PhysicsWorld::AddImpulse(Entity entity, const glm::vec3& impulse)
	{
		auto* body = m_Impl->MutableBody(entity, "AddImpulse");
		if (body != nullptr)
			m_Impl->System->GetBodyInterface().AddImpulse(body->ID, ToJolt(impulse));
	}

	void PhysicsWorld::AddTorque(Entity entity, const glm::vec3& torque)
	{
		auto* body = m_Impl->MutableBody(entity, "AddTorque");
		if (body != nullptr)
			m_Impl->System->GetBodyInterface().AddTorque(body->ID, ToJolt(torque));
	}

	void PhysicsWorld::SetLinearVelocity(Entity entity, const glm::vec3& velocity)
	{
		auto* body = m_Impl->MutableBody(entity, "SetLinearVelocity");
		if (body != nullptr)
			m_Impl->System->GetBodyInterface().SetLinearVelocity(body->ID, ToJolt(velocity));
	}

	glm::vec3 PhysicsWorld::GetLinearVelocity(Entity entity) const
	{
		if (!m_Impl->Owns(entity))
			return glm::vec3(0.0f);
		if (auto character = m_Impl->Characters.find(entity.GetUUID()); character != m_Impl->Characters.end())
			return character->second.ActualVelocity;
		auto* body = m_Impl->FindBody(entity);
		if (body == nullptr)
			return glm::vec3(0.0f);
		return FromJolt(m_Impl->System->GetBodyInterface().GetLinearVelocity(body->ID));
	}

	void PhysicsWorld::SetAngularVelocity(Entity entity, const glm::vec3& velocity)
	{
		auto* body = m_Impl->MutableBody(entity, "SetAngularVelocity");
		if (body != nullptr)
			m_Impl->System->GetBodyInterface().SetAngularVelocity(body->ID, ToJolt(velocity));
	}

	glm::vec3 PhysicsWorld::GetAngularVelocity(Entity entity) const
	{
		auto* body = m_Impl->FindBody(entity);
		if (body == nullptr)
			return glm::vec3(0.0f);
		return FromJolt(m_Impl->System->GetBodyInterface().GetAngularVelocity(body->ID));
	}

	bool PhysicsWorld::Impl::IsSleeping(const BodyRecord* record) const
	{
		return record != nullptr && record->Settings.Type != RigidBodyType::Static && !System->GetBodyInterface().IsActive(record->ID);
	}

	bool PhysicsWorld::IsBodySleeping(Entity entity) const
	{
		// FindBody(Entity) only answers for this world's scene.
		return m_Impl->IsSleeping(m_Impl->FindBody(entity));
	}

	const PhysicsLayers& PhysicsWorld::GetLayers() const
	{
		return m_Impl->Layers;
	}

	glm::vec3 PhysicsWorld::GetGravity() const
	{
		return FromJolt(m_Impl->System->GetGravity());
	}

	void PhysicsWorld::SetGravity(const glm::vec3& gravity)
	{
		m_Impl->System->SetGravity(ToJolt(gravity));
	}

	void PhysicsWorld::ClearMeshShapeCache()
	{
		PhysicsInternal::ClearMeshShapeCache();
	}

	uint64_t PhysicsWorld::GetMeshShapeCookCount()
	{
		return PhysicsInternal::GetMeshShapeCookCount();
	}

	uint32_t PhysicsWorld::GetBodyCount() const
	{
		uint32_t count = static_cast<uint32_t>(m_Impl->ScenePtr->GetRegistry().view<Impl::BodyRecord>().size());
		for (const auto& [id, record] : m_Impl->Ragdolls)
			if (record.Doll)
				count += static_cast<uint32_t>(record.Doll->GetBodyCount());
		return count;
	}

}
