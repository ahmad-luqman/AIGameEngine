#include "Basalt/Physics/PhysicsWorld.h"

#include "Basalt/Physics/ColliderShapes.h"
#include "Basalt/Physics/MeshShapeCache.h"
#include "Basalt/Physics/PhysicsWorldImpl.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scripting/ScriptEngine.h"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

namespace Basalt {

	using namespace PhysicsInternal;

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

	void PhysicsWorld::Impl::WritePose(Scene* scene, Entity entity, const glm::vec3& position, const glm::quat& rotation)
	{
		glm::vec3 oldPosition;
		glm::quat oldRotation;
		glm::vec3 scale;
		Math::DecomposeTransform(scene->GetWorldTransform(entity), oldPosition, oldRotation, scale);
		TransformComponent& transform = entity.GetTransform();
		const glm::vec3 localScale = transform.Scale;
		scene->SetWorldTransform(entity, Math::ComposeTransform(position, rotation, scale));
		for (int axis = 0; axis < 3; axis++)
			transform.Scale[axis] = std::copysign(std::abs(localScale[axis]), transform.Scale[axis]);
	}

	void PhysicsWorld::Impl::MarkRescaledDirty(Scene* scene)
	{
		// Relative, so tiny and huge scales are treated alike; decomposing the same transform again only
		// differs in the last bits, far below this.
		auto rescaled = [](const glm::vec3& scale, const glm::vec3& built) {
			return glm::any(glm::greaterThan(glm::abs(scale - built), glm::abs(built) * 1e-4f + glm::vec3(1e-7f)));
		};
		auto check = [&](entt::entity handle, const glm::vec3& built) {
			Entity entity(handle, scene);
			glm::vec3 position;
			glm::quat rotation;
			glm::vec3 scale;
			// A degenerate transform keeps the old shape; RecreateBody would only refuse to build a new one.
			if (Math::DecomposeTransform(scene->GetWorldTransform(entity), position, rotation, scale) && rescaled(scale, built))
				DirtyEntities.Insert(entity.GetUUID());
		};
		for (entt::entity handle : scene->GetAllEntitiesWith<RigidBodyComponent>())
		{
			if (auto it = Bodies.find(Entity(handle, scene).GetUUID()); it != Bodies.end())
				check(handle, it->second.BuiltScale);
		}
		for (entt::entity handle : scene->GetAllEntitiesWith<CharacterControllerComponent>())
		{
			if (auto it = Characters.find(Entity(handle, scene).GetUUID()); it != Characters.end())
				check(handle, it->second.BuiltScale);
		}
	}

	std::optional<ContactInfo> PhysicsWorld::Impl::SeparationContact(UUID first, UUID second, float fixedStep) const
	{
		auto bodyOf = [this](UUID uuid) {
			if (auto body = Bodies.find(uuid); body != Bodies.end())
				return body->second.ID;
			if (auto character = Characters.find(uuid); character != Characters.end())
				return character->second.Character->GetInnerBodyID();
			return JPH::BodyID();
		};
		const JPH::BodyID bodyA = bodyOf(first);
		const JPH::BodyID bodyB = bodyOf(second);
		if (bodyA.IsInvalid() || bodyB.IsInvalid())
			return std::nullopt;

		const JPH::BodyInterface& bodies = System->GetBodyInterface();
		const JPH::TransformedShape shapeA = bodies.GetTransformedShape(bodyA);
		const JPH::TransformedShape shapeB = bodies.GetTransformedShape(bodyB);
		// They parted during the last step, so they are about one step of relative motion apart (plus Jolt's
		// speculative contact distance; a character's inner body reports no velocity, hence the margin).
		const float relativeSpeed = (bodies.GetLinearVelocity(bodyA) - bodies.GetLinearVelocity(bodyB)).Length();
		JPH::CollideShapeSettings settings;
		settings.mMaxSeparationDistance = 0.25f + 2.0f * relativeSpeed * fixedStep;
		// The least separated pair of parts.
		JPH::ClosestHitCollisionCollector<JPH::CollideShapeCollector> collector;
		shapeB.CollideShape(shapeA.mShape, shapeA.GetShapeScale(), shapeA.GetCenterOfMassTransform(), settings, JPH::RVec3::sZero(), collector);
		if (!collector.HadHit())
			return std::nullopt;
		// Shape 1 is A's: the penetration axis points from A toward B, and the normal from B toward A.
		const JPH::CollideShapeResult& hit = collector.mHit;
		ContactInfo contact;
		contact.Point = FromJolt(JPH::Vec3(0.5f * (hit.mContactPointOn1 + hit.mContactPointOn2)));
		contact.Normal = FromJolt(-hit.mPenetrationAxis.NormalizedOr(JPH::Vec3::sZero()));
		return contact;
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
		if (auto it = id ? Bodies.find(id->ID) : Bodies.end(); it != Bodies.end() && it->second.BorrowedMesh == borrowed)
			return;
		if (auto it = id ? Characters.find(id->ID) : Characters.end(); it != Characters.end() && it->second.BorrowedMesh == borrowed)
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
		// Adding or removing a controller swaps a body for a character, so it rebuilds like a collider change.
		Wire<CharacterControllerComponent, Changed, &Impl::OnCharacterChanged, Changed>(registry, connect);
		Wire<JointComponent, &Impl::OnJointChanged, &Impl::OnJointChanged, &Impl::OnJointDestroyed>(registry, connect);
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
		m_Impl->CharacterListener.Owner = m_Impl.get();
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
		m_Impl->WireSignals(registry, true);
	}

	PhysicsWorld::~PhysicsWorld()
	{
		entt::registry& registry = m_Scene->GetRegistry();
		m_Impl->WireSignals(registry, false);

		for (auto& [uuid, joint] : m_Impl->Joints)
			m_Impl->System->RemoveConstraint(joint.Constraint);
		m_Impl->Joints.clear();
		// Characters remove their inner bodies, so they go while the system exists.
		m_Impl->CharacterCollision.mCharacters.clear();
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
		m_Impl->DirtyEntities.Clear();
		m_Impl->DirtyCharacters.Clear();
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
		std::vector<std::string> warnings;
		auto warn = [&warnings](const std::string& warning) { warnings.push_back(warning); };
		auto reportWarnings = [&]() { impl.BodyWarnings.Report(entity, warnings); };

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

		const bool anyOverride = std::ranges::any_of(colliders.Materials, &ColliderMaterial::Override);
		impl.ContactListener.Materials[bodyID.GetIndex()] = { rigidBody.FrictionCombine, rigidBody.RestitutionCombine, colliders.Materials, anyOverride };

		Impl::BodyRecord record;
		record.ID = bodyID;
		record.Type = rigidBody.Type;
		record.IsTrigger = rigidBody.IsTrigger;
		record.BorrowedMesh = colliders.BorrowedMesh;
		record.LastPosition = position;
		record.LastRotation = rotation;
		record.BuiltScale = scale;
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
		m_Impl->CharacterWarnings.Forget(entity.GetUUID());
		m_Impl->DirtyJoints.erase(entity.GetUUID());
		m_Impl->JointWarnings.Forget(entity.GetUUID());
		m_Impl->BodyWarnings.Forget(entity.GetUUID());
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

		// Rebuild bodies whose physics components or scale changed since the last step.
		impl.MarkRescaledDirty(m_Scene);
		for (UUID uuid : impl.DirtyEntities.Take())
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
		for (UUID uuid : impl.DirtyCharacters.Take())
		{
			if (Entity entity = m_Scene->GetEntityByUUID(uuid))
				ApplyCharacterSettings(entity);
		}
		RebuildDirtyJoints();

		m_Accumulator += ts.GetSeconds();
		uint32_t steps = 0;
		while (m_Accumulator >= fixedStep && steps < settings.MaxStepsPerFrame)
		{
			// Push entity-side transform changes into the simulation, in registry order: activating bodies
			// orders Jolt's active list, so a hash order would differ between standard libraries.
			for (entt::entity handle : m_Scene->GetAllEntitiesWith<RigidBodyComponent>())
			{
				Entity entity(handle, m_Scene);
				auto found = impl.Bodies.find(entity.GetUUID());
				if (found == impl.Bodies.end())
					continue;
				Impl::BodyRecord& record = found->second;

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

			// Write simulated poses of dynamic bodies back to their entities, parents before children: a
			// child's local transform is derived from its parent's world transform, which must already be
			// this step's.
			std::vector<std::pair<uint32_t, Entity>> movedBodies;
			for (entt::entity handle : m_Scene->GetAllEntitiesWith<RigidBodyComponent>())
			{
				Entity entity(handle, m_Scene);
				auto found = impl.Bodies.find(entity.GetUUID());
				if (found != impl.Bodies.end() && found->second.Type == RigidBodyType::Dynamic)
					movedBodies.emplace_back(m_Scene->GetDepth(entity), entity);
			}
			std::ranges::stable_sort(movedBodies, {}, &std::pair<uint32_t, Entity>::first);
			for (auto& [depth, entity] : movedBodies)
			{
				Impl::BodyRecord& record = impl.Bodies.at(entity.GetUUID());

				JPH::RVec3 joltPosition;
				JPH::Quat joltRotation;
				bodies.GetPositionAndRotation(record.ID, joltPosition, joltRotation);
				const glm::vec3 position = FromJolt(joltPosition);
				const glm::quat rotation = glm::normalize(FromJolt(joltRotation));

				Impl::WritePose(m_Scene, entity, position, rotation);
				record.LastPosition = position;
				record.LastRotation = rotation;
			}

			CheckBrokenJoints(fixedStep);
			DispatchContacts(fixedStep);
		}

		// Never let a long stall build an unbounded backlog of steps.
		if (steps == settings.MaxStepsPerFrame)
			m_Accumulator = std::min(m_Accumulator, fixedStep);
	}

	void PhysicsWorld::DispatchContacts(float fixedStep)
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
		// The strongest new contact of each pair, relative to the pair's first (lower UUID) entity (begin and
		// enter events).
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
			std::optional<ContactInfo> contact;
			if (isActive)
			{
				if (auto found = newContacts.find(pair); found != newContacts.end())
					contact = found->second;
			}
			else
			{
				contact = impl.SeparationContact(UUID(pair.first), UUID(pair.second), fixedStep);
			}
			scriptEngine->OnContactEvent(type, entityA, entityB, contact);
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
