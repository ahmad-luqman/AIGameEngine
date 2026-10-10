#include "Basalt/Physics/PhysicsWorld.h"

#include "Basalt/Physics/PhysicsWorldImpl.h"
#include "Basalt/Physics/ColliderShapes.h"
#include "Basalt/Physics/MeshShapeCache.h"
#include "Basalt/Project/Project.h"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CollideShape.h>

#include <algorithm>
#include <map>
#include <tuple>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace Basalt {

	bool PhysicsWorld::Impl::IsTrigger(UUID uuid) const
	{
		auto it = Bodies.find(uuid);
		return it != Bodies.end() && it->second.IsTrigger;
	}

	void PhysicsWorld::Impl::RemoveBody(Scene* scene, UUID uuid)
	{
		auto it = Bodies.find(uuid);
		if (it == Bodies.end())
			return;
		const bool trigger = it->second.IsTrigger;
		RemoveJointsOfBody(uuid);
		JPH::BodyInterface& bodies = System->GetBodyInterface();
		BodyToEntity.erase(it->second.ID.GetIndexAndSequenceNumber());
		// Jolt reuses the index for the next body; whatever creates it must not inherit these modes.
		ContactListener.Materials[it->second.ID.GetIndex()] = {};
		bodies.RemoveBody(it->second.ID);
		bodies.DestroyBody(it->second.ID);
		Bodies.erase(it);
		EndContacts(scene, uuid, trigger);
	}

	void PhysicsWorld::Impl::EndContacts(Scene* scene, UUID uuid, bool trigger)
	{
		const uint64_t id = static_cast<uint64_t>(uuid);
		std::set<EntityPair> ended;
		for (auto key = ActiveContacts.begin(); key != ActiveContacts.end();)
		{
			if (std::get<0>(*key) != id && std::get<2>(*key) != id)
			{
				++key;
				continue;
			}
			const EntityPair pair(std::get<0>(*key), std::get<2>(*key));
			if (--PairCounts[pair] <= 0)
			{
				PairCounts.erase(pair);
				ended.insert(pair);
			}
			SuspendedContacts.erase(*key);
			key = ActiveContacts.erase(key);
		}

		ScriptEngine* scriptEngine = scene->GetScriptEngine();
		for (const EntityPair& pair : ended)
		{
			const UUID other = pair.first == id ? UUID(pair.second) : UUID(pair.first);
			const bool isTrigger = trigger || IsTrigger(other);
			Entity a = scene->GetEntityByUUID(pair.first);
			Entity b = scene->GetEntityByUUID(pair.second);
			if (scriptEngine && a && b)
				scriptEngine->OnContactEvent(isTrigger ? ContactEventType::TriggerExit : ContactEventType::CollisionEnd, a, b);
		}
	}

	void PhysicsWorld::Impl::OnPhysicsComponentChanged(entt::registry& registry, entt::entity entity)
	{
		if (const auto* id = registry.try_get<IDComponent>(entity))
			DirtyEntities.insert(id->ID);
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
		if (auto it = id ? Bodies.find(id->ID) : Bodies.end(); it != Bodies.end() && it->second.BorrowedMesh == borrowed)
			return;
		if (auto it = id ? Characters.find(id->ID) : Characters.end(); it != Characters.end() && it->second.BorrowedMesh == borrowed)
			return;
		OnMeshChanged(registry, entity);
	}

	PhysicsWorld::PhysicsWorld(Scene* scene)
		: m_Impl(CreateScope<Impl>())
		, m_Scene(scene)
	{
		AcquireJolt();

		constexpr uint32_t MaxBodies = 65536;
		constexpr uint32_t NumBodyMutexes = 0; // Jolt default
		constexpr uint32_t MaxBodyPairs = 65536;
		constexpr uint32_t MaxContactConstraints = 16384;

		m_Impl->TempAllocator = CreateScope<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
		const int threadCount = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
		m_Impl->JobSystem = CreateScope<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threadCount);

		if (const auto& layers = scene->GetPhysicsLayers())
			m_Impl->Layers = *layers;
		else if (const Ref<Project>& project = Project::GetActive())
			m_Impl->Layers = project->GetConfig().Physics;
		m_Impl->ObjectLayerPairFilter.SetLayers(m_Impl->Layers);

		m_Impl->System = CreateScope<JPH::PhysicsSystem>();
		m_Impl->System->Init(MaxBodies, NumBodyMutexes, MaxBodyPairs, MaxContactConstraints,
							 m_Impl->BroadPhaseLayerInterface, m_Impl->ObjectVsBroadPhaseLayerFilter, m_Impl->ObjectLayerPairFilter);
		m_Impl->ContactListener.Materials.resize(MaxBodies);
		m_Impl->ContactListener.MinVelocityForRestitution = m_Impl->System->GetPhysicsSettings().mMinVelocityForRestitution;
		m_Impl->System->SetContactListener(&m_Impl->ContactListener);
		// Bodies joined by a joint do not collide with each other unless the joint asks for it.
		m_Impl->System->SetSimCollideBodyVsBody([impl = m_Impl.get()](const JPH::Body& body1, const JPH::Body& body2, JPH::Mat44Arg transform1, JPH::Mat44Arg transform2,
																	  JPH::CollideShapeSettings& settings, JPH::CollideShapeCollector& collector, const JPH::ShapeFilter& filter) {
			if (!impl->IgnoredPairs.empty())
			{
				const uint64_t a = body1.GetUserData();
				const uint64_t b = body2.GetUserData();
				if (impl->IgnoredPairs.contains({ std::min(a, b), std::max(a, b) }))
					return;
			}
			JPH::PhysicsSystem::sDefaultSimCollideBodyVsBody(body1, body2, transform1, transform2, settings, collector, filter);
		});
		SetGravity(scene->GetPhysicsSettings().Gravity);

		entt::registry& registry = scene->GetRegistry();
		Impl& impl = *m_Impl;
		registry.on_construct<RigidBodyComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<RigidBodyComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<RigidBodyComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<BoxColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<BoxColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<BoxColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<SphereColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<SphereColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<SphereColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<CapsuleColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<CapsuleColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<CapsuleColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<MeshColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<MeshColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<MeshColliderComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<MeshComponent>().connect<&Impl::OnMeshChanged>(impl);
		registry.on_destroy<MeshComponent>().connect<&Impl::OnMeshChanged>(impl);
		registry.on_update<MeshComponent>().connect<&Impl::OnMeshUpdated>(impl);
		// Adding or removing a controller swaps a body for a character, so it rebuilds like a collider change.
		registry.on_construct<CharacterControllerComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<CharacterControllerComponent>().connect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<CharacterControllerComponent>().connect<&Impl::OnCharacterChanged>(impl);
		registry.on_construct<JointComponent>().connect<&Impl::OnJointChanged>(impl);
		registry.on_update<JointComponent>().connect<&Impl::OnJointChanged>(impl);
		registry.on_destroy<JointComponent>().connect<&Impl::OnJointDestroyed>(impl);
	}

	PhysicsWorld::~PhysicsWorld()
	{
		entt::registry& registry = m_Scene->GetRegistry();
		Impl& impl = *m_Impl;
		registry.on_construct<RigidBodyComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<RigidBodyComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<RigidBodyComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<BoxColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<BoxColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<BoxColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<SphereColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<SphereColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<SphereColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<CapsuleColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<CapsuleColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<CapsuleColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<MeshColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<MeshColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<MeshColliderComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_construct<MeshComponent>().disconnect<&Impl::OnMeshChanged>(impl);
		registry.on_destroy<MeshComponent>().disconnect<&Impl::OnMeshChanged>(impl);
		registry.on_update<MeshComponent>().disconnect<&Impl::OnMeshUpdated>(impl);
		registry.on_construct<CharacterControllerComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_destroy<CharacterControllerComponent>().disconnect<&Impl::OnPhysicsComponentChanged>(impl);
		registry.on_update<CharacterControllerComponent>().disconnect<&Impl::OnCharacterChanged>(impl);
		registry.on_construct<JointComponent>().disconnect<&Impl::OnJointChanged>(impl);
		registry.on_update<JointComponent>().disconnect<&Impl::OnJointChanged>(impl);
		registry.on_destroy<JointComponent>().disconnect<&Impl::OnJointDestroyed>(impl);

		for (auto& [uuid, joint] : m_Impl->Joints)
			m_Impl->System->RemoveConstraint(joint.Constraint);
		m_Impl->Joints.clear();
		// Characters remove their inner bodies, so they go while the system exists.
		m_Impl->Characters.clear();
		JPH::BodyInterface& bodies = m_Impl->System->GetBodyInterface();
		for (auto& [uuid, record] : m_Impl->Bodies)
		{
			bodies.RemoveBody(record.ID);
			bodies.DestroyBody(record.ID);
		}
		m_Impl->Bodies.clear();
		m_Impl->System.reset();
		m_Impl->JobSystem.reset();
		m_Impl->TempAllocator.reset();
		m_Impl.reset();

		ReleaseJolt();
	}

	void PhysicsWorld::Start()
	{
		auto view = m_Scene->GetAllEntitiesWith<RigidBodyComponent>();
		m_Impl->Starting = true;
		for (entt::entity handle : view)
			RecreateBody({ handle, m_Scene });
		for (entt::entity handle : m_Scene->GetAllEntitiesWith<CharacterControllerComponent>())
			RecreateCharacter({ handle, m_Scene });
		m_Impl->Starting = false;
		m_Impl->DirtyEntities.clear();
		m_Impl->DirtyCharacters.clear();
		// Joints need both of their bodies, so they are built after every body exists.
		for (entt::entity handle : m_Scene->GetAllEntitiesWith<JointComponent>())
			m_Impl->DirtyJoints.insert(Entity(handle, m_Scene).GetUUID());
		RebuildDirtyJoints();
		m_Impl->System->OptimizeBroadPhase();
	}

	void PhysicsWorld::RecreateBody(Entity entity)
	{
		Impl& impl = *m_Impl;
		JPH::BodyInterface& bodies = impl.System->GetBodyInterface();
		JPH::Vec3 previousLinearVelocity = JPH::Vec3::sZero();
		JPH::Vec3 previousAngularVelocity = JPH::Vec3::sZero();

		if (entity)
		{
			auto existing = impl.Bodies.find(entity.GetUUID());
			if (existing != impl.Bodies.end())
			{
				// Rebuilding (e.g. after a collider or rigid body change) keeps the body moving.
				previousLinearVelocity = bodies.GetLinearVelocity(existing->second.ID);
				previousAngularVelocity = bodies.GetAngularVelocity(existing->second.ID);
				impl.RemoveBody(m_Scene, entity.GetUUID());
			}
		}

		// A character controller replaces the body (RecreateCharacter reports the ignored RigidBody).
		if (!entity || !entity.HasComponent<RigidBodyComponent>() || entity.HasComponent<CharacterControllerComponent>())
			return;

		const auto& rigidBody = entity.GetComponent<RigidBodyComponent>();
		glm::vec3 position;
		glm::quat rotation;
		glm::vec3 scale;
		if (!Math::DecomposeTransform(m_Scene->GetWorldTransform(entity), position, rotation, scale))
		{
			BS_CORE_WARN("Physics: entity '{}' has a degenerate transform; no body created", entity.GetName());
			return;
		}

		JPH::EMotionType motionType = JPH::EMotionType::Static;
		if (rigidBody.Type == RigidBodyType::Dynamic)
			motionType = JPH::EMotionType::Dynamic;
		else if (rigidBody.Type == RigidBodyType::Kinematic)
			motionType = JPH::EMotionType::Kinematic;

		// Problems with the body's settings, joined with "; ". Logged when they differ from the last ones
		// logged for this entity, so a script that sets the component every frame does not flood the log.
		std::string warnings;
		auto warn = [&warnings](const std::string& warning) {
			warnings += (warnings.empty() ? "" : "; ") + warning;
		};
		auto reportWarnings = [&]() {
			if (warnings.empty())
			{
				impl.LoggedBodyWarnings.erase(entity.GetUUID());
			}
			else if (std::string& logged = impl.LoggedBodyWarnings[entity.GetUUID()]; logged != warnings)
			{
				logged = warnings;
				BS_CORE_WARN("Physics: entity '{}': {}", entity.GetName(), warnings);
			}
		};

		// Jolt cannot simulate a triangle mesh on a dynamic body, so those always use the convex hull.
		const ColliderShape colliders = BuildColliderShape(entity, scale, 1.0f, motionType == JPH::EMotionType::Dynamic ? "a dynamic body" : nullptr, rigidBody.IsTrigger, warn);
		if (!colliders.Shape)
		{
			warn("no body created");
			reportWarnings();
			return;
		}
		const bool hasTriangleMesh = colliders.HasTriangleMesh;

		const bool moving = motionType != JPH::EMotionType::Static;
		uint32_t layer = 0;
		if (const auto index = impl.Layers.Find(rigidBody.Layer))
			layer = *index;
		else
			warn("unknown physics layer '" + rigidBody.Layer + "', using Default");
		JPH::BodyCreationSettings settings(colliders.Shape, ToJolt(position), ToJolt(rotation), motionType, MakeObjectLayer(moving, layer));
		settings.mUserData = static_cast<uint64_t>(entity.GetUUID());
		// The combine modes pass these on unchecked: negative friction inverts Jolt's friction clamp and
		// restitution above 1 adds energy on every bounce.
		settings.mFriction = std::max(rigidBody.Friction, 0.0f);
		if (rigidBody.Friction < 0.0f)
			warn("Friction must not be negative, using 0");
		settings.mRestitution = std::clamp(rigidBody.Restitution, 0.0f, 1.0f);
		if (rigidBody.Restitution < 0.0f || rigidBody.Restitution > 1.0f)
			warn("Restitution must be between 0 and 1, clamped");
		settings.mLinearDamping = rigidBody.LinearDamping;
		settings.mAngularDamping = rigidBody.AngularDamping;
		settings.mGravityFactor = rigidBody.GravityFactor;
		settings.mIsSensor = rigidBody.IsTrigger;
		// Kinematic triggers must also see static bodies; kinematic bodies are otherwise skipped vs static.
		settings.mCollideKinematicVsNonDynamic = rigidBody.IsTrigger;
		if (rigidBody.FixedRotation && motionType == JPH::EMotionType::Dynamic)
			settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
		// Jolt only sweeps dynamic, non-sensor bodies.
		if (rigidBody.Continuous)
		{
			if (motionType == JPH::EMotionType::Dynamic && !rigidBody.IsTrigger)
				settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
			else
				warn("Continuous only affects dynamic bodies that are not triggers");
		}
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
			BS_CORE_ERROR("Physics: body limit reached; no body created for '{}'", entity.GetName());
			return;
		}

		impl.ContactListener.Materials[bodyID.GetIndex()] = { rigidBody.FrictionCombine, rigidBody.RestitutionCombine };

		Impl::BodyRecord record;
		record.ID = bodyID;
		record.Type = rigidBody.Type;
		record.IsTrigger = rigidBody.IsTrigger;
		record.BorrowedMesh = colliders.BorrowedMesh;
		record.LastPosition = position;
		record.LastRotation = rotation;
		impl.Bodies[entity.GetUUID()] = record;
		impl.BodyToEntity[bodyID.GetIndexAndSequenceNumber()] = entity.GetUUID();
		if (motionType == JPH::EMotionType::Dynamic)
		{
			bodies.SetLinearVelocity(bodyID, previousLinearVelocity);
			bodies.SetAngularVelocity(bodyID, previousAngularVelocity);
		}
		// Joints that could not be built without this body (or were removed with its old one) can be now.
		if (!impl.Starting)
			MarkJointsDirty(entity.GetUUID());
	}

	void PhysicsWorld::OnEntityDestroyed(Entity entity)
	{
		m_Impl->RemoveBody(m_Scene, entity.GetUUID());
		m_Impl->RemoveCharacter(m_Scene, entity.GetUUID());
		m_Impl->RemoveJoint(entity.GetUUID());
		m_Impl->DirtyEntities.erase(entity.GetUUID());
		m_Impl->DirtyCharacters.erase(entity.GetUUID());
		m_Impl->LoggedCharacterWarnings.erase(entity.GetUUID());
		m_Impl->DirtyJoints.erase(entity.GetUUID());
		m_Impl->LoggedJointWarnings.erase(entity.GetUUID());
		m_Impl->LoggedBodyWarnings.erase(entity.GetUUID());
	}

	bool PhysicsWorld::HasBody(Entity entity) const
	{
		return entity && m_Impl->Bodies.contains(entity.GetUUID());
	}

	void PhysicsWorld::Step(Timestep ts)
	{
		Impl& impl = *m_Impl;
		JPH::BodyInterface& bodies = impl.System->GetBodyInterface();
		const PhysicsSettings& settings = m_Scene->GetPhysicsSettings();
		const float fixedStep = settings.FixedTimestep > 0.0f ? settings.FixedTimestep : 1.0f / 60.0f;

		// Rebuild bodies whose physics components changed since the last step.
		if (!impl.DirtyEntities.empty())
		{
			const std::unordered_set<UUID> dirty = std::move(impl.DirtyEntities);
			impl.DirtyEntities.clear();
			for (UUID uuid : dirty)
			{
				Entity entity = m_Scene->GetEntityByUUID(uuid);
				if (entity)
				{
					RecreateBody(entity);
					RecreateCharacter(entity);
				}
				else
				{
					impl.RemoveBody(m_Scene, uuid);
					impl.RemoveCharacter(m_Scene, uuid);
				}
			}
		}
		if (!impl.DirtyCharacters.empty())
		{
			const std::unordered_set<UUID> dirty = std::move(impl.DirtyCharacters);
			impl.DirtyCharacters.clear();
			for (UUID uuid : dirty)
			{
				if (Entity entity = m_Scene->GetEntityByUUID(uuid))
					ApplyCharacterSettings(entity);
			}
		}
		RebuildDirtyJoints();

		m_Accumulator += ts.GetSeconds();
		uint32_t steps = 0;
		while (m_Accumulator >= fixedStep && steps < settings.MaxStepsPerFrame)
		{
			// Push entity-side transform changes into the simulation.
			for (auto& [uuid, record] : impl.Bodies)
			{
				Entity entity = m_Scene->GetEntityByUUID(uuid);
				if (!entity)
					continue;

				glm::vec3 position;
				glm::quat rotation;
				glm::vec3 scale;
				if (!Math::DecomposeTransform(m_Scene->GetWorldTransform(entity), position, rotation, scale))
					continue;

				const bool moved = glm::any(glm::greaterThan(glm::abs(position - record.LastPosition), glm::vec3(1e-5f))) || glm::abs(glm::dot(rotation, record.LastRotation)) < 1.0f - 1e-6f;
				if (record.Type == RigidBodyType::Kinematic)
				{
					bodies.MoveKinematic(record.ID, ToJolt(position), ToJolt(rotation), fixedStep);
				}
				else if (moved)
				{
					const JPH::EActivation activation = record.Type == RigidBodyType::Dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
					bodies.SetPositionAndRotation(record.ID, ToJolt(position), ToJolt(rotation), activation);
				}
				record.LastPosition = position;
				record.LastRotation = rotation;
			}

			// Characters move before the bodies step, as Jolt expects.
			UpdateCharacters(fixedStep);

			impl.System->Update(fixedStep, 1, impl.TempAllocator.get(), impl.JobSystem.get());
			m_Accumulator -= fixedStep;
			m_StepCount++;
			steps++;

			// Write simulated poses of dynamic bodies back to their entities.
			for (auto& [uuid, record] : impl.Bodies)
			{
				if (record.Type != RigidBodyType::Dynamic)
					continue;
				Entity entity = m_Scene->GetEntityByUUID(uuid);
				if (!entity)
					continue;

				JPH::RVec3 joltPosition;
				JPH::Quat joltRotation;
				bodies.GetPositionAndRotation(record.ID, joltPosition, joltRotation);
				const glm::vec3 position = FromJolt(joltPosition);
				const glm::quat rotation = glm::normalize(FromJolt(joltRotation));

				glm::vec3 oldPosition;
				glm::quat oldRotation;
				glm::vec3 scale;
				Math::DecomposeTransform(m_Scene->GetWorldTransform(entity), oldPosition, oldRotation, scale);
				m_Scene->SetWorldTransform(entity, Math::ComposeTransform(position, rotation, scale));
				record.LastPosition = position;
				record.LastRotation = rotation;
			}

			CheckBrokenJoints(fixedStep);
			DispatchContacts();
		}

		// Never let a long stall build an unbounded backlog of steps.
		if (steps == settings.MaxStepsPerFrame)
			m_Accumulator = std::min(m_Accumulator, fixedStep);
	}

	void PhysicsWorld::DispatchContacts()
	{
		Impl& impl = *m_Impl;
		JPH::BodyInterface& bodies = impl.System->GetBodyInterface();

		std::vector<RawContactEvent> events;
		{
			std::scoped_lock lock(impl.ContactListener.Mutex);
			events.swap(impl.ContactListener.Events);
		}
		// Worker threads record events in scheduling order; sort so callbacks are deterministic.
		std::sort(events.begin(), events.end(), [](const RawContactEvent& a, const RawContactEvent& b) { return a.Tie() < b.Tie(); });

		// A body that falls asleep reports its contacts as removed; it is still touching, so keep them.
		auto isSleeping = [&](uint32_t body) {
			auto entity = impl.BodyToEntity.find(body);
			if (entity == impl.BodyToEntity.end())
				return false;
			auto record = impl.Bodies.find(entity->second);
			return record != impl.Bodies.end() && record->second.Type != RigidBodyType::Static && !bodies.IsActive(record->second.ID);
		};

		std::map<Impl::EntityPair, bool> touched; // pair -> was active before this step
		// The strongest new contact of each pair, relative to the pair's first (lower UUID) entity.
		std::map<Impl::EntityPair, ContactInfo> newContacts;
		for (const RawContactEvent& event : events)
		{
			auto first = impl.BodyToEntity.find(event.Body1);
			auto second = impl.BodyToEntity.find(event.Body2);
			if (first == impl.BodyToEntity.end() || second == impl.BodyToEntity.end())
				continue;

			uint64_t a = static_cast<uint64_t>(first->second);
			uint64_t b = static_cast<uint64_t>(second->second);
			uint32_t subA = event.SubShape1;
			uint32_t subB = event.SubShape2;
			// The event's normal points from body 1 toward body 2; ContactInfo's from the second entity
			// toward the first.
			ContactInfo contact = event.Contact.Flipped();
			if (b < a)
			{
				std::swap(a, b);
				std::swap(subA, subB);
				contact = event.Contact;
			}
			const Impl::ContactKey key(a, subA, b, subB);
			const Impl::EntityPair pair(a, b);

			if (!event.Added && (isSleeping(event.Body1) || isSleeping(event.Body2)))
			{
				if (impl.ActiveContacts.contains(key))
					impl.SuspendedContacts.insert(key);
				continue;
			}

			touched.emplace(pair, impl.PairCounts.contains(pair));
			if (event.Added)
			{
				auto [best, inserted] = newContacts.try_emplace(pair, contact);
				if (!inserted && contact.Impulse > best->second.Impulse)
					best->second = contact;
				impl.SuspendedContacts.erase(key);
				if (impl.ActiveContacts.insert(key).second)
					impl.PairCounts[pair]++;
			}
			else if (impl.ActiveContacts.erase(key) > 0)
			{
				if (--impl.PairCounts[pair] <= 0)
					impl.PairCounts.erase(pair);
			}
		}

		// Suspended contacts whose bodies are awake again and were not re-reported this step have ended.
		auto isEntitySleeping = [&](uint64_t uuid) {
			auto record = impl.Bodies.find(uuid);
			return record != impl.Bodies.end() && record->second.Type != RigidBodyType::Static && !bodies.IsActive(record->second.ID);
		};
		for (auto key = impl.SuspendedContacts.begin(); key != impl.SuspendedContacts.end();)
		{
			if (isEntitySleeping(std::get<0>(*key)) || isEntitySleeping(std::get<2>(*key)))
			{
				++key;
				continue;
			}
			const Impl::EntityPair pair(std::get<0>(*key), std::get<2>(*key));
			touched.emplace(pair, impl.PairCounts.contains(pair));
			if (impl.ActiveContacts.erase(*key) > 0 && --impl.PairCounts[pair] <= 0)
				impl.PairCounts.erase(pair);
			key = impl.SuspendedContacts.erase(key);
		}

		ScriptEngine* scriptEngine = m_Scene->GetScriptEngine();
		if (!scriptEngine)
			return;
		for (const auto& [pair, wasActive] : touched)
		{
			const bool isActive = impl.PairCounts.contains(pair);
			if (wasActive == isActive)
				continue;
			Entity entityA = m_Scene->GetEntityByUUID(pair.first);
			Entity entityB = m_Scene->GetEntityByUUID(pair.second);
			if (!entityA || !entityB)
				continue;
			const bool trigger = impl.IsTrigger(pair.first) || impl.IsTrigger(pair.second);
			ContactEventType type;
			if (trigger)
				type = isActive ? ContactEventType::TriggerEnter : ContactEventType::TriggerExit;
			else
				type = isActive ? ContactEventType::CollisionBegin : ContactEventType::CollisionEnd;
			auto contact = newContacts.find(pair);
			scriptEngine->OnContactEvent(type, entityA, entityB, contact != newContacts.end() ? contact->second : ContactInfo{});
		}
	}

	void PhysicsWorld::AddForce(Entity entity, const glm::vec3& force)
	{
		if (m_Impl->WarnIfCharacter(entity, "AddForce"))
			return;
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it != m_Impl->Bodies.end())
			m_Impl->System->GetBodyInterface().AddForce(it->second.ID, ToJolt(force));
	}

	void PhysicsWorld::AddImpulse(Entity entity, const glm::vec3& impulse)
	{
		if (m_Impl->WarnIfCharacter(entity, "AddImpulse"))
			return;
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it != m_Impl->Bodies.end())
			m_Impl->System->GetBodyInterface().AddImpulse(it->second.ID, ToJolt(impulse));
	}

	void PhysicsWorld::AddTorque(Entity entity, const glm::vec3& torque)
	{
		if (m_Impl->WarnIfCharacter(entity, "AddTorque"))
			return;
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it != m_Impl->Bodies.end())
			m_Impl->System->GetBodyInterface().AddTorque(it->second.ID, ToJolt(torque));
	}

	void PhysicsWorld::SetLinearVelocity(Entity entity, const glm::vec3& velocity)
	{
		if (m_Impl->WarnIfCharacter(entity, "SetLinearVelocity"))
			return;
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it != m_Impl->Bodies.end())
			m_Impl->System->GetBodyInterface().SetLinearVelocity(it->second.ID, ToJolt(velocity));
	}

	glm::vec3 PhysicsWorld::GetLinearVelocity(Entity entity) const
	{
		if (auto character = m_Impl->Characters.find(entity.GetUUID()); character != m_Impl->Characters.end())
			return character->second.ActualVelocity;
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it == m_Impl->Bodies.end())
			return glm::vec3(0.0f);
		return FromJolt(m_Impl->System->GetBodyInterface().GetLinearVelocity(it->second.ID));
	}

	void PhysicsWorld::SetAngularVelocity(Entity entity, const glm::vec3& velocity)
	{
		if (m_Impl->WarnIfCharacter(entity, "SetAngularVelocity"))
			return;
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it != m_Impl->Bodies.end())
			m_Impl->System->GetBodyInterface().SetAngularVelocity(it->second.ID, ToJolt(velocity));
	}

	glm::vec3 PhysicsWorld::GetAngularVelocity(Entity entity) const
	{
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it == m_Impl->Bodies.end())
			return glm::vec3(0.0f);
		return FromJolt(m_Impl->System->GetBodyInterface().GetAngularVelocity(it->second.ID));
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
		return static_cast<uint32_t>(m_Impl->Bodies.size());
	}

}
