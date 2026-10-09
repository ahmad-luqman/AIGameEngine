#include "Basalt/Physics/PhysicsWorld.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scripting/ScriptEngine.h"

// Jolt.h must precede every other Jolt header.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Basalt {

	namespace {

		// ---------------------------------------------------------------------------------------
		// Jolt global state (factory, type registry) shared by every PhysicsWorld.
		// ---------------------------------------------------------------------------------------
		std::mutex s_JoltMutex;
		uint32_t s_JoltUsers = 0;

		void JoltTrace(const char* format, ...)
		{
			char buffer[1024];
			va_list args;
			va_start(args, format);
			std::vsnprintf(buffer, sizeof(buffer), format, args);
			va_end(args);
			BS_CORE_TRACE("[Jolt] {}", buffer);
		}

		void AcquireJolt()
		{
			std::scoped_lock lock(s_JoltMutex);
			if (s_JoltUsers++ > 0)
				return;
			JPH::RegisterDefaultAllocator();
			JPH::Trace = JoltTrace;
			JPH::Factory::sInstance = new JPH::Factory();
			JPH::RegisterTypes();
		}

		void ReleaseJolt()
		{
			std::scoped_lock lock(s_JoltMutex);
			if (--s_JoltUsers > 0)
				return;
			JPH::UnregisterTypes();
			delete JPH::Factory::sInstance;
			JPH::Factory::sInstance = nullptr;
		}

		// ---------------------------------------------------------------------------------------
		// Object layers: bit 20 = moving, bits 16-19 = collision layer index, bits 0-15 = collision mask.
		// ---------------------------------------------------------------------------------------
		constexpr uint32_t MovingBit = 1u << 20;
		constexpr uint32_t MaxCollisionLayers = 16;

		JPH::ObjectLayer MakeObjectLayer(bool moving, uint32_t layer, uint32_t mask)
		{
			return static_cast<JPH::ObjectLayer>((moving ? MovingBit : 0u) | ((layer & 0xFu) << 16) | (mask & 0xFFFFu));
		}

		bool IsMovingLayer(JPH::ObjectLayer layer)
		{
			return (layer & MovingBit) != 0;
		}
		uint32_t LayerIndex(JPH::ObjectLayer layer)
		{
			return (layer >> 16) & 0xFu;
		}
		uint32_t LayerMask(JPH::ObjectLayer layer)
		{
			return layer & 0xFFFFu;
		}

		namespace BroadPhaseLayers {
			constexpr JPH::BroadPhaseLayer NonMoving(0);
			constexpr JPH::BroadPhaseLayer Moving(1);
			constexpr uint32_t Count = 2;
		}

		class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
		{
		public:
			JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::Count; }
			JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
			{
				return IsMovingLayer(layer) ? BroadPhaseLayers::Moving : BroadPhaseLayers::NonMoving;
			}
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
			const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
			{
				return layer == BroadPhaseLayers::Moving ? "Moving" : "NonMoving";
			}
#endif
		};

		class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
		{
		public:
			bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhaseLayer) const override
			{
				// Static bodies never need to test against other static bodies.
				return IsMovingLayer(layer) || broadPhaseLayer == BroadPhaseLayers::Moving;
			}
		};

		class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
		{
		public:
			bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
			{
				if (!IsMovingLayer(a) && !IsMovingLayer(b))
					return false;
				return ((1u << LayerIndex(a)) & LayerMask(b)) != 0 && ((1u << LayerIndex(b)) & LayerMask(a)) != 0;
			}
		};

		JPH::Vec3 ToJolt(const glm::vec3& v)
		{
			return JPH::Vec3(v.x, v.y, v.z);
		}
		JPH::Quat ToJolt(const glm::quat& q)
		{
			return JPH::Quat(q.x, q.y, q.z, q.w);
		}
		glm::vec3 FromJolt(const JPH::Vec3& v)
		{
			return { v.GetX(), v.GetY(), v.GetZ() };
		}
		glm::quat FromJolt(const JPH::Quat& q)
		{
			return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
		}

		struct RawContactEvent
		{
			bool Added = false;
			uint32_t Body1 = 0;
			uint32_t SubShape1 = 0;
			uint32_t Body2 = 0;
			uint32_t SubShape2 = 0;

			auto Tie() const { return std::tie(Added, Body1, SubShape1, Body2, SubShape2); }
		};

		// Called from Jolt worker threads: only records events, which the main thread dispatches.
		class ContactListenerImpl final : public JPH::ContactListener
		{
		public:
			void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings&) override
			{
				std::scoped_lock lock(Mutex);
				Events.push_back({ true, body1.GetID().GetIndexAndSequenceNumber(), manifold.mSubShapeID1.GetValue(), body2.GetID().GetIndexAndSequenceNumber(), manifold.mSubShapeID2.GetValue() });
			}

			void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
			{
				std::scoped_lock lock(Mutex);
				Events.push_back({ false, pair.GetBody1ID().GetIndexAndSequenceNumber(), pair.GetSubShapeID1().GetValue(), pair.GetBody2ID().GetIndexAndSequenceNumber(), pair.GetSubShapeID2().GetValue() });
			}

			std::mutex Mutex;
			std::vector<RawContactEvent> Events;
		};

		// Whether a joint change needs a new constraint. Only the fields a live Jolt constraint can update
		// are exempt, so a field added later rebuilds the joint by default instead of being ignored in play.
		// (Toggling UseLimits rebuilds too: it changes how a distance joint's rest length is chosen.)
		bool NeedsRebuild(const JointComponent& built, const JointComponent& current)
		{
			JointComponent structural = current;
			structural.LimitMin = built.LimitMin;
			structural.LimitMax = built.LimitMax;
			structural.MotorMode = built.MotorMode;
			structural.MotorTarget = built.MotorTarget;
			structural.MotorMaxForce = built.MotorMaxForce;
			structural.BreakForce = built.BreakForce;
			structural.BreakTorque = built.BreakTorque;
			structural.EnableCollision = built.EnableCollision;
			return structural != built;
		}

		const char* JointTypeName(JointType type)
		{
			switch (type)
			{
				case JointType::Fixed:
					return "fixed";
				case JointType::Point:
					return "point";
				case JointType::Hinge:
					return "hinge";
				case JointType::Slider:
					return "slider";
				case JointType::Distance:
					return "distance";
			}
			return "unknown";
		}

		// Limits Jolt accepts for the joint (it asserts on others). Hinge and slider limits must contain
		// the rest pose (0); distance limits are lengths. Changed values are reported in warnings.
		std::pair<float, float> SanitizeLimits(const JointComponent& joint, std::vector<std::string>& warnings)
		{
			if (!joint.UseLimits)
				return { 0.0f, 0.0f };
			switch (joint.Type)
			{
				case JointType::Hinge:
				case JointType::Slider:
				{
					const bool hinge = joint.Type == JointType::Hinge;
					const float range = hinge ? 180.0f : FLT_MAX;
					const float min = std::clamp(joint.LimitMin, -range, 0.0f);
					const float max = std::clamp(joint.LimitMax, 0.0f, range);
					if (min != joint.LimitMin || max != joint.LimitMax)
						warnings.emplace_back(fmt::format("{} limits [{}, {}] must satisfy {} (the rest pose is 0); clamped to [{}, {}]", JointTypeName(joint.Type), joint.LimitMin,
														  joint.LimitMax, hinge ? "-180 <= LimitMin <= 0 <= LimitMax <= 180 degrees" : "LimitMin <= 0 <= LimitMax", min, max));
					return { min, max };
				}
				case JointType::Distance:
				{
					const float min = std::max(joint.LimitMin, 0.0f);
					const float max = std::max(joint.LimitMax, min);
					if (min != joint.LimitMin || max != joint.LimitMax)
						warnings.emplace_back(fmt::format("distance limits [{}, {}] must satisfy 0 <= LimitMin <= LimitMax; clamped to [{}, {}]", joint.LimitMin, joint.LimitMax, min, max));
					if (max <= 0.0f)
						warnings.emplace_back("distance limits with LimitMax <= 0 pull the anchors together");
					return { min, max };
				}
				case JointType::Fixed:
				case JointType::Point:
					warnings.emplace_back(fmt::format("UseLimits has no effect on {} joints", JointTypeName(joint.Type)));
					break;
			}
			return { 0.0f, 0.0f };
		}

		JPH::EMotorState ToJoltMotorState(JointMotorMode mode)
		{
			switch (mode)
			{
				case JointMotorMode::Velocity:
					return JPH::EMotorState::Velocity;
				case JointMotorMode::Position:
					return JPH::EMotorState::Position;
				case JointMotorMode::Off:
					break;
			}
			return JPH::EMotorState::Off;
		}

		// Raycasts skip triggers and (optionally) one entity.
		class RaycastBodyFilter final : public JPH::BodyFilter
		{
		public:
			explicit RaycastBodyFilter(uint64_t ignoreUserData)
				: m_Ignore(ignoreUserData)
			{
			}

			bool ShouldCollideLocked(const JPH::Body& body) const override
			{
				return !body.IsSensor() && (m_Ignore == 0 || body.GetUserData() != m_Ignore);
			}

		private:
			uint64_t m_Ignore = 0;
		};

	}

	struct PhysicsWorld::Impl
	{
		struct BodyRecord
		{
			JPH::BodyID ID;
			RigidBodyType Type = RigidBodyType::Static;
			bool IsTrigger = false;
			// Pose last written to / read from the entity, used to detect transforms changed by scripts.
			glm::vec3 LastPosition = { 0.0f, 0.0f, 0.0f };
			glm::quat LastRotation = { 1.0f, 0.0f, 0.0f, 0.0f };
		};

		struct JointRecord
		{
			JPH::Ref<JPH::TwoBodyConstraint> Constraint;
			// The component the constraint was built from (plus later in-place updates). Its Type always
			// matches the constraint's subtype; ConnectedEntity is 0 when attached to the world.
			JointComponent Settings;
		};

		Scope<JPH::TempAllocatorImpl> TempAllocator;
		Scope<JPH::JobSystemThreadPool> JobSystem;
		BroadPhaseLayerInterfaceImpl BroadPhaseLayerInterface;
		ObjectVsBroadPhaseLayerFilterImpl ObjectVsBroadPhaseLayerFilter;
		ObjectLayerPairFilterImpl ObjectLayerPairFilter;
		ContactListenerImpl ContactListener;
		Scope<JPH::PhysicsSystem> System;

		std::unordered_map<UUID, BodyRecord> Bodies;
		std::unordered_map<uint32_t, UUID> BodyToEntity;
		std::unordered_set<UUID> DirtyEntities;
		// Keyed by the entity that owns the JointComponent.
		std::unordered_map<UUID, JointRecord> Joints;
		std::unordered_set<UUID> DirtyJoints;
		// The warnings last logged for each joint, so repeats are not logged again.
		std::unordered_map<UUID, std::vector<std::string>> LoggedJointWarnings;
		// Set while Start() creates every body; it marks all joints dirty itself afterwards.
		bool Starting = false;
		// Jolt reports contacts per sub-shape pair; entities see one begin/end per entity pair. Keys are
		// (entity A, sub-shape A, entity B, sub-shape B) with A < B; PairCounts counts keys per entity pair.
		using ContactKey = std::tuple<uint64_t, uint32_t, uint64_t, uint32_t>;
		using EntityPair = std::pair<uint64_t, uint64_t>;
		std::set<ContactKey> ActiveContacts;
		std::map<EntityPair, int> PairCounts;
		// Entity pairs (lower UUID first) joined by a joint with EnableCollision off. Read by Jolt worker
		// threads during Update, so it only changes between steps.
		std::set<EntityPair> IgnoredPairs;
		// Contacts whose removal was reported because a body fell asleep. They stay active; once both bodies
		// are awake again, a contact that Jolt does not re-report has really ended.
		std::set<ContactKey> SuspendedContacts;

		bool IsTrigger(UUID uuid) const
		{
			auto it = Bodies.find(uuid);
			return it != Bodies.end() && it->second.IsTrigger;
		}

		void UpdateIgnoredPairs()
		{
			IgnoredPairs.clear();
			for (const auto& [owner, joint] : Joints)
			{
				if (joint.Settings.ConnectedEntity == 0 || joint.Settings.EnableCollision)
					continue;
				const uint64_t a = static_cast<uint64_t>(owner);
				const uint64_t b = static_cast<uint64_t>(joint.Settings.ConnectedEntity);
				IgnoredPairs.emplace(std::min(a, b), std::max(a, b));
			}
		}

		void RemoveJoint(UUID owner)
		{
			auto it = Joints.find(owner);
			if (it == Joints.end())
				return;
			System->RemoveConstraint(it->second.Constraint);
			Joints.erase(it);
			UpdateIgnoredPairs();
		}

		// Jolt constraints point at their bodies, so they must go before either body is destroyed. They are
		// rebuilt before the next step if both bodies still exist then (e.g. a body that was recreated).
		void RemoveJointsOfBody(UUID uuid)
		{
			for (auto it = Joints.begin(); it != Joints.end();)
			{
				if (it->first != uuid && it->second.Settings.ConnectedEntity != uuid)
				{
					++it;
					continue;
				}
				System->RemoveConstraint(it->second.Constraint);
				DirtyJoints.insert(it->first);
				it = Joints.erase(it);
			}
			UpdateIgnoredPairs();
		}

		// Removes a body. Contacts it had are ended now (Jolt's removal callbacks for a destroyed body can
		// no longer be mapped to an entity), and surviving entities are notified.
		void RemoveBody(Scene* scene, UUID uuid)
		{
			auto it = Bodies.find(uuid);
			if (it == Bodies.end())
				return;
			const bool trigger = it->second.IsTrigger;
			RemoveJointsOfBody(uuid);
			JPH::BodyInterface& bodies = System->GetBodyInterface();
			BodyToEntity.erase(it->second.ID.GetIndexAndSequenceNumber());
			bodies.RemoveBody(it->second.ID);
			bodies.DestroyBody(it->second.ID);
			Bodies.erase(it);

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

		void OnPhysicsComponentChanged(entt::registry& registry, entt::entity entity)
		{
			if (const auto* id = registry.try_get<IDComponent>(entity))
				DirtyEntities.insert(id->ID);
		}

		void OnJointChanged(entt::registry& registry, entt::entity entity)
		{
			if (const auto* id = registry.try_get<IDComponent>(entity))
				DirtyJoints.insert(id->ID);
		}

		void OnJointDestroyed(entt::registry& registry, entt::entity entity)
		{
			if (const auto* id = registry.try_get<IDComponent>(entity))
			{
				RemoveJoint(id->ID);
				DirtyJoints.erase(id->ID);
				LoggedJointWarnings.erase(id->ID);
			}
		}
	};

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

		m_Impl->System = CreateScope<JPH::PhysicsSystem>();
		m_Impl->System->Init(MaxBodies, NumBodyMutexes, MaxBodyPairs, MaxContactConstraints,
							 m_Impl->BroadPhaseLayerInterface, m_Impl->ObjectVsBroadPhaseLayerFilter, m_Impl->ObjectLayerPairFilter);
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
		registry.on_construct<JointComponent>().disconnect<&Impl::OnJointChanged>(impl);
		registry.on_update<JointComponent>().disconnect<&Impl::OnJointChanged>(impl);
		registry.on_destroy<JointComponent>().disconnect<&Impl::OnJointDestroyed>(impl);

		for (auto& [uuid, joint] : m_Impl->Joints)
			m_Impl->System->RemoveConstraint(joint.Constraint);
		m_Impl->Joints.clear();
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
		m_Impl->Starting = false;
		m_Impl->DirtyEntities.clear();
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

		if (!entity || !entity.HasComponent<RigidBodyComponent>())
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
		scale = glm::abs(scale);

		JPH::StaticCompoundShapeSettings compound;
		uint32_t shapeCount = 0;
		auto addShape = [&](const JPH::ShapeSettings::ShapeResult& result, const glm::vec3& offset) {
			if (result.HasError())
			{
				BS_CORE_WARN("Physics: invalid collider on '{}': {}", entity.GetName(), result.GetError().c_str());
				return;
			}
			compound.AddShape(ToJolt(offset * scale), JPH::Quat::sIdentity(), result.Get());
			shapeCount++;
		};

		constexpr float MinExtent = 0.001f;
		if (const auto* box = entity.TryGetComponent<BoxColliderComponent>())
		{
			const glm::vec3 halfExtents = glm::max(box->HalfExtents * scale, glm::vec3(MinExtent));
			const float convexRadius = std::min(JPH::cDefaultConvexRadius, glm::min(halfExtents.x, glm::min(halfExtents.y, halfExtents.z)) * 0.5f);
			addShape(JPH::BoxShapeSettings(ToJolt(halfExtents), convexRadius).Create(), box->Offset);
		}
		if (const auto* sphere = entity.TryGetComponent<SphereColliderComponent>())
		{
			const float radius = std::max(sphere->Radius * glm::max(scale.x, glm::max(scale.y, scale.z)), MinExtent);
			addShape(JPH::SphereShapeSettings(radius).Create(), sphere->Offset);
		}
		if (const auto* capsule = entity.TryGetComponent<CapsuleColliderComponent>())
		{
			const float radius = std::max(capsule->Radius * glm::max(scale.x, scale.z), MinExtent);
			const float halfHeight = std::max(capsule->HalfHeight * scale.y, MinExtent);
			addShape(JPH::CapsuleShapeSettings(halfHeight, radius).Create(), capsule->Offset);
		}

		if (shapeCount == 0)
		{
			BS_CORE_WARN("Physics: entity '{}' has a RigidBodyComponent but no valid collider; no body created", entity.GetName());
			return;
		}

		JPH::ShapeSettings::ShapeResult shapeResult = compound.Create();
		if (shapeResult.HasError())
		{
			BS_CORE_WARN("Physics: failed to build shape for '{}': {}", entity.GetName(), shapeResult.GetError().c_str());
			return;
		}

		JPH::EMotionType motionType = JPH::EMotionType::Static;
		if (rigidBody.Type == RigidBodyType::Dynamic)
			motionType = JPH::EMotionType::Dynamic;
		else if (rigidBody.Type == RigidBodyType::Kinematic)
			motionType = JPH::EMotionType::Kinematic;

		const bool moving = motionType != JPH::EMotionType::Static;
		const uint32_t layer = std::min(rigidBody.Layer, MaxCollisionLayers - 1);
		JPH::BodyCreationSettings settings(shapeResult.Get(), ToJolt(position), ToJolt(rotation), motionType, MakeObjectLayer(moving, layer, rigidBody.CollisionMask));
		settings.mUserData = static_cast<uint64_t>(entity.GetUUID());
		settings.mFriction = rigidBody.Friction;
		settings.mRestitution = rigidBody.Restitution;
		settings.mLinearDamping = rigidBody.LinearDamping;
		settings.mAngularDamping = rigidBody.AngularDamping;
		settings.mGravityFactor = rigidBody.GravityFactor;
		settings.mIsSensor = rigidBody.IsTrigger;
		// Kinematic triggers must also see static bodies; kinematic bodies are otherwise skipped vs static.
		settings.mCollideKinematicVsNonDynamic = rigidBody.IsTrigger;
		if (rigidBody.FixedRotation && motionType == JPH::EMotionType::Dynamic)
			settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
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

		Impl::BodyRecord record;
		record.ID = bodyID;
		record.Type = rigidBody.Type;
		record.IsTrigger = rigidBody.IsTrigger;
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

	void PhysicsWorld::MarkJointsDirty(UUID uuid)
	{
		for (entt::entity handle : m_Scene->GetAllEntitiesWith<JointComponent>())
		{
			Entity entity(handle, m_Scene);
			if (entity.GetUUID() == uuid || entity.GetComponent<JointComponent>().ConnectedEntity == uuid)
				m_Impl->DirtyJoints.insert(entity.GetUUID());
		}
	}

	void PhysicsWorld::OnEntityDestroyed(Entity entity)
	{
		m_Impl->RemoveBody(m_Scene, entity.GetUUID());
		m_Impl->RemoveJoint(entity.GetUUID());
		m_Impl->DirtyEntities.erase(entity.GetUUID());
		m_Impl->DirtyJoints.erase(entity.GetUUID());
		m_Impl->LoggedJointWarnings.erase(entity.GetUUID());
	}

	void PhysicsWorld::RebuildDirtyJoints()
	{
		Impl& impl = *m_Impl;
		if (impl.DirtyJoints.empty())
			return;
		// Walk the registry rather than the (unordered) dirty set so constraints are added in a stable
		// order: Jolt solves them in that order, and replays must not depend on hash-set iteration.
		for (entt::entity handle : m_Scene->GetAllEntitiesWith<JointComponent>())
		{
			Entity entity(handle, m_Scene);
			// An entity destroyed during this frame's scripts is still here until the end of the frame.
			if (!impl.DirtyJoints.contains(entity.GetUUID()) || m_Scene->IsEntityPendingDestruction(entity))
				continue;
			// Scripts drive motors by setting the component every frame. Rebuilding would reset the
			// constraint's rest pose and warm start, so only structural changes rebuild it.
			auto existing = impl.Joints.find(entity.GetUUID());
			const JointComponent& joint = entity.GetComponent<JointComponent>();
			std::vector<std::string> warnings;
			if (existing != impl.Joints.end() && !NeedsRebuild(existing->second.Settings, joint))
			{
				// Rewriting the same values must not wake the bodies.
				if (joint == existing->second.Settings)
					continue;
				ApplyJointSettings(entity, warnings);
			}
			else
			{
				CreateJoint(entity, warnings);
			}
			ReportJointWarnings(entity, std::move(warnings));
		}
		impl.DirtyJoints.clear();
		impl.UpdateIgnoredPairs();
	}

	void PhysicsWorld::ReportJointWarnings(Entity entity, std::vector<std::string> warnings)
	{
		// Messages name the offending values, so the same messages mean the same problem.
		auto& logged = m_Impl->LoggedJointWarnings;
		auto it = logged.find(entity.GetUUID());
		if (warnings.empty())
		{
			if (it != logged.end())
				logged.erase(it);
			return;
		}
		if (it != logged.end() && it->second == warnings)
			return;
		for (const std::string& warning : warnings)
			BS_CORE_WARN("Physics: joint on '{}': {}", entity.GetName(), warning);
		logged[entity.GetUUID()] = std::move(warnings);
	}

	void PhysicsWorld::ApplyJointSettings(Entity entity, std::vector<std::string>& warnings)
	{
		Impl::JointRecord& record = m_Impl->Joints.at(entity.GetUUID());
		const JointComponent& joint = entity.GetComponent<JointComponent>();
		record.Settings = joint;

		const auto [limitMin, limitMax] = SanitizeLimits(joint, warnings);
		const bool motorAllowed = joint.Type == JointType::Hinge || joint.Type == JointType::Slider;
		if (joint.MotorMode != JointMotorMode::Off && !motorAllowed)
			warnings.emplace_back(fmt::format("only hinge and slider joints have motors; MotorMode is ignored on {} joints", JointTypeName(joint.Type)));
		if (joint.MotorMaxForce < 0.0f)
			warnings.emplace_back(fmt::format("MotorMaxForce {} is negative; using 0", joint.MotorMaxForce));
		if (joint.BreakTorque > 0.0f && (joint.Type == JointType::Point || joint.Type == JointType::Distance))
			warnings.emplace_back(fmt::format("BreakTorque has no effect on {} joints, which hold no torque", JointTypeName(joint.Type)));
		const JPH::EMotorState motorState = ToJoltMotorState(joint.MotorMode);
		const float motorLimit = std::max(joint.MotorMaxForce, 0.0f);

		switch (joint.Type)
		{
			case JointType::Hinge:
			{
				BS_CORE_ASSERT(record.Constraint->GetSubType() == JPH::EConstraintSubType::Hinge, "joint record out of sync with its constraint");
				auto* hinge = static_cast<JPH::HingeConstraint*>(record.Constraint.GetPtr());
				if (joint.UseLimits)
					hinge->SetLimits(glm::radians(limitMin), glm::radians(limitMax));
				hinge->GetMotorSettings().SetTorqueLimit(motorLimit);
				hinge->SetMotorState(motorState);
				if (motorState == JPH::EMotorState::Velocity)
				{
					hinge->SetTargetAngularVelocity(glm::radians(joint.MotorTarget));
				}
				else if (motorState == JPH::EMotorState::Position)
				{
					if (joint.MotorTarget < -180.0f || joint.MotorTarget > 180.0f)
						warnings.emplace_back("hinge position motor targets must be within [-180, 180] degrees; clamped");
					hinge->SetTargetAngle(glm::radians(std::clamp(joint.MotorTarget, -180.0f, 180.0f)));
				}
				break;
			}
			case JointType::Slider:
			{
				BS_CORE_ASSERT(record.Constraint->GetSubType() == JPH::EConstraintSubType::Slider, "joint record out of sync with its constraint");
				auto* slider = static_cast<JPH::SliderConstraint*>(record.Constraint.GetPtr());
				if (joint.UseLimits)
					slider->SetLimits(limitMin, limitMax);
				slider->GetMotorSettings().SetForceLimit(motorLimit);
				slider->SetMotorState(motorState);
				if (motorState == JPH::EMotorState::Velocity)
					slider->SetTargetVelocity(joint.MotorTarget);
				else if (motorState == JPH::EMotorState::Position)
					slider->SetTargetPosition(joint.MotorTarget);
				break;
			}
			case JointType::Distance:
				BS_CORE_ASSERT(record.Constraint->GetSubType() == JPH::EConstraintSubType::Distance, "joint record out of sync with its constraint");
				if (joint.UseLimits)
					static_cast<JPH::DistanceConstraint*>(record.Constraint.GetPtr())->SetDistance(limitMin, limitMax);
				break;
			case JointType::Fixed:
			case JointType::Point:
				break;
		}

		// Sleeping bodies would ignore a new motor target or limit.
		JPH::BodyInterface& bodies = m_Impl->System->GetBodyInterface();
		for (const JPH::Body* body : { record.Constraint->GetBody1(), record.Constraint->GetBody2() })
		{
			if (body && !body->IsStatic())
				bodies.ActivateBody(body->GetID());
		}
	}

	void PhysicsWorld::CheckBrokenJoints(float fixedStep)
	{
		Impl& impl = *m_Impl;
		if (impl.Joints.empty())
			return;

		// Constraint lambdas are the impulses applied during the last step; divided by the step they are
		// the force (N) and torque (N·m) the joint needed to hold. Hinge and slider limits and motors act
		// along the joint's free axis, so they add to the force or torque the joint carries.
		struct BrokenJoint
		{
			UUID Owner;
			float Force = 0.0f;
			float Torque = 0.0f;
		};
		auto exceeds = [](float value, float limit) { return limit > 0.0f && value > limit; };
		auto combine = [](float perpendicular, float axial) { return std::sqrt(perpendicular * perpendicular + axial * axial); };
		std::vector<BrokenJoint> broken;
		for (entt::entity handle : m_Scene->GetAllEntitiesWith<JointComponent>())
		{
			Entity entity(handle, m_Scene);
			auto it = impl.Joints.find(entity.GetUUID());
			if (it == impl.Joints.end())
				continue;
			const Impl::JointRecord& record = it->second;
			const float breakForce = record.Settings.BreakForce;
			const float breakTorque = record.Settings.BreakTorque;
			if (breakForce <= 0.0f && breakTorque <= 0.0f)
				continue;

			float forceImpulse = 0.0f;
			float torqueImpulse = 0.0f;
			const JPH::Constraint* constraint = record.Constraint.GetPtr();
			switch (record.Settings.Type)
			{
				case JointType::Fixed:
				{
					const auto* fixed = static_cast<const JPH::FixedConstraint*>(constraint);
					forceImpulse = fixed->GetTotalLambdaPosition().Length();
					torqueImpulse = fixed->GetTotalLambdaRotation().Length();
					break;
				}
				case JointType::Point:
					forceImpulse = static_cast<const JPH::PointConstraint*>(constraint)->GetTotalLambdaPosition().Length();
					break;
				case JointType::Hinge:
				{
					const auto* hinge = static_cast<const JPH::HingeConstraint*>(constraint);
					forceImpulse = hinge->GetTotalLambdaPosition().Length();
					torqueImpulse = combine(hinge->GetTotalLambdaRotation().Length(), hinge->GetTotalLambdaRotationLimits() + hinge->GetTotalLambdaMotor());
					break;
				}
				case JointType::Slider:
				{
					const auto* slider = static_cast<const JPH::SliderConstraint*>(constraint);
					forceImpulse = combine(slider->GetTotalLambdaPosition().Length(), slider->GetTotalLambdaPositionLimits() + slider->GetTotalLambdaMotor());
					torqueImpulse = slider->GetTotalLambdaRotation().Length();
					break;
				}
				case JointType::Distance:
					forceImpulse = std::abs(static_cast<const JPH::DistanceConstraint*>(constraint)->GetTotalLambdaPosition());
					break;
			}
			const float force = forceImpulse / fixedStep;
			const float torque = torqueImpulse / fixedStep;
			if (exceeds(force, breakForce) || exceeds(torque, breakTorque))
				broken.push_back({ entity.GetUUID(), force, torque });
		}

		// A broken joint is gone for good: its component is removed, so the scene state (and scene.hash)
		// records the break and replays stay consistent.
		ScriptEngine* scriptEngine = m_Scene->GetScriptEngine();
		for (const BrokenJoint& joint : broken)
		{
			// An earlier OnJointBreak callback may have removed this joint or its entity.
			Entity entity = m_Scene->GetEntityByUUID(joint.Owner);
			auto it = impl.Joints.find(joint.Owner);
			if (!entity || it == impl.Joints.end() || !entity.HasComponent<JointComponent>())
				continue;
			const JointComponent& settings = it->second.Settings;
			// The measured values help tune thresholds.
			BS_CORE_INFO("Physics: joint on '{}' broke (force {:.1f} N, BreakForce {}; torque {:.1f} N·m, BreakTorque {})", entity.GetName(), joint.Force, settings.BreakForce, joint.Torque, settings.BreakTorque);
			const UUID connectedID = settings.ConnectedEntity;
			entity.RemoveComponent<JointComponent>();
			if (scriptEngine)
				scriptEngine->OnJointBroken(entity, m_Scene->GetEntityByUUID(connectedID));
		}
	}

	void PhysicsWorld::CreateJoint(Entity entity, std::vector<std::string>& warnings)
	{
		Impl& impl = *m_Impl;
		impl.RemoveJoint(entity.GetUUID());
		const JointComponent& joint = entity.GetComponent<JointComponent>();

		// A RigidBody without a valid collider has no body either; say which part is missing.
		auto describeMissingBody = [](Entity e) {
			return e.HasComponent<RigidBodyComponent>() ? "has a RigidBody but no valid collider" : "has no RigidBody";
		};
		auto self = impl.Bodies.find(entity.GetUUID());
		if (self == impl.Bodies.end())
		{
			warnings.emplace_back(fmt::format("not built: the entity {}", describeMissingBody(entity)));
			return;
		}

		JPH::BodyID bodyIDs[2] = { JPH::BodyID(), self->second.ID };
		RigidBodyType connectedType = RigidBodyType::Static;
		Entity connected;
		if (joint.ConnectedEntity != 0)
		{
			connected = m_Scene->GetEntityByUUID(joint.ConnectedEntity);
			if (!connected || connected == entity)
			{
				warnings.emplace_back(fmt::format("not built: it connects to {} entity {}", connected ? "its own" : "a missing", static_cast<uint64_t>(joint.ConnectedEntity)));
				return;
			}
			auto other = impl.Bodies.find(joint.ConnectedEntity);
			if (other == impl.Bodies.end())
			{
				warnings.emplace_back(fmt::format("not built: the connected entity '{}' {}", connected.GetName(), describeMissingBody(connected)));
				return;
			}
			bodyIDs[0] = other->second.ID;
			connectedType = other->second.Type;
		}
		// Jolt only moves dynamic bodies; a constraint between static and kinematic bodies does nothing.
		if (self->second.Type != RigidBodyType::Dynamic && connectedType != RigidBodyType::Dynamic)
		{
			warnings.emplace_back("not built: it connects no dynamic body (only static, kinematic or the world) and would have no effect");
			return;
		}

		// The joint frame in world space. Both bodies share it at creation, so their current relative pose
		// becomes the joint's rest pose. The axis turns with the body's rotation only: Jolt bodies are
		// unscaled, so a scaled world matrix would skew it under non-uniform scale.
		const glm::mat4 world = m_Scene->GetWorldTransform(entity);
		glm::vec3 position;
		glm::quat rotation;
		glm::vec3 scale;
		if (!Math::DecomposeTransform(world, position, rotation, scale))
		{
			warnings.emplace_back("not built: the entity has a degenerate transform");
			return;
		}
		const glm::vec3 anchor = glm::vec3(world * glm::vec4(joint.Anchor, 1.0f));
		glm::vec3 axis = joint.Axis;
		if (glm::length(axis) < 1e-6f)
		{
			if (joint.Type == JointType::Hinge || joint.Type == JointType::Slider)
				warnings.emplace_back("Axis is zero; using local Y");
			axis = glm::vec3(0.0f, 1.0f, 0.0f);
		}
		const JPH::Vec3 joltAxis = ToJolt(glm::normalize(rotation * axis));
		const JPH::Vec3 joltNormal = joltAxis.GetNormalizedPerpendicular();

		// Limits, motors and break thresholds are applied by ApplyJointSettings below.
		JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
		switch (joint.Type)
		{
			case JointType::Fixed:
			{
				auto* fixed = new JPH::FixedConstraintSettings();
				fixed->mPoint1 = fixed->mPoint2 = ToJolt(anchor);
				settings = fixed;
				break;
			}
			case JointType::Point:
			{
				auto* point = new JPH::PointConstraintSettings();
				point->mPoint1 = point->mPoint2 = ToJolt(anchor);
				settings = point;
				break;
			}
			case JointType::Hinge:
			{
				auto* hinge = new JPH::HingeConstraintSettings();
				hinge->mPoint1 = hinge->mPoint2 = ToJolt(anchor);
				hinge->mHingeAxis1 = hinge->mHingeAxis2 = joltAxis;
				hinge->mNormalAxis1 = hinge->mNormalAxis2 = joltNormal;
				settings = hinge;
				break;
			}
			case JointType::Slider:
			{
				auto* slider = new JPH::SliderConstraintSettings();
				slider->mPoint1 = slider->mPoint2 = ToJolt(anchor);
				slider->mSliderAxis1 = slider->mSliderAxis2 = joltAxis;
				slider->mNormalAxis1 = slider->mNormalAxis2 = joltNormal;
				settings = slider;
				break;
			}
			case JointType::Distance:
			{
				auto* distance = new JPH::DistanceConstraintSettings();
				const glm::vec3 connectedAnchor = connected ? glm::vec3(m_Scene->GetWorldTransform(connected) * glm::vec4(joint.ConnectedAnchor, 1.0f)) : joint.ConnectedAnchor;
				distance->mPoint1 = ToJolt(connectedAnchor);
				distance->mPoint2 = ToJolt(anchor);
				// Jolt keeps the starting distance (its default min/max of -1 mean "current"); limits replace it.
				settings = distance;
				break;
			}
		}

		// Body 1 is the connected body (or the world), body 2 this entity's.
		const int bodyCount = connected ? 2 : 1;
		JPH::BodyLockMultiWrite lock(impl.System->GetBodyLockInterface(), connected ? bodyIDs : bodyIDs + 1, bodyCount);
		JPH::Body* body1 = connected ? lock.GetBody(0) : &JPH::Body::sFixedToWorld;
		JPH::Body* body2 = lock.GetBody(bodyCount - 1);
		if (!body1 || !body2)
		{
			// Both bodies are in Bodies, so Jolt should always find them.
			BS_CORE_ERROR("Physics: joint on '{}' not built: its bodies could not be locked", entity.GetName());
			return;
		}

		Impl::JointRecord& record = impl.Joints[entity.GetUUID()];
		record.Constraint = settings->Create(*body1, *body2);
		record.Settings = joint;
		impl.System->AddConstraint(record.Constraint);
		lock.ReleaseLocks();
		ApplyJointSettings(entity, warnings);
	}

	bool PhysicsWorld::HasJoint(Entity entity) const
	{
		return entity && m_Impl->Joints.contains(entity.GetUUID());
	}

	std::optional<float> PhysicsWorld::GetJointPosition(Entity entity) const
	{
		if (!entity)
			return std::nullopt;
		auto it = m_Impl->Joints.find(entity.GetUUID());
		if (it == m_Impl->Joints.end())
			return std::nullopt;
		const JPH::Constraint* constraint = it->second.Constraint.GetPtr();
		switch (it->second.Settings.Type)
		{
			case JointType::Hinge:
				return glm::degrees(static_cast<const JPH::HingeConstraint*>(constraint)->GetCurrentAngle());
			case JointType::Slider:
				return static_cast<const JPH::SliderConstraint*>(constraint)->GetCurrentPosition();
			case JointType::Fixed:
			case JointType::Point:
			case JointType::Distance:
				break;
		}
		return std::nullopt;
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
				}
				else
				{
					impl.RemoveBody(m_Scene, uuid);
				}
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
			if (b < a)
			{
				std::swap(a, b);
				std::swap(subA, subB);
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
			scriptEngine->OnContactEvent(type, entityA, entityB);
		}
	}

	void PhysicsWorld::AddForce(Entity entity, const glm::vec3& force)
	{
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it != m_Impl->Bodies.end())
			m_Impl->System->GetBodyInterface().AddForce(it->second.ID, ToJolt(force));
	}

	void PhysicsWorld::AddImpulse(Entity entity, const glm::vec3& impulse)
	{
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it != m_Impl->Bodies.end())
			m_Impl->System->GetBodyInterface().AddImpulse(it->second.ID, ToJolt(impulse));
	}

	void PhysicsWorld::AddTorque(Entity entity, const glm::vec3& torque)
	{
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it != m_Impl->Bodies.end())
			m_Impl->System->GetBodyInterface().AddTorque(it->second.ID, ToJolt(torque));
	}

	void PhysicsWorld::SetLinearVelocity(Entity entity, const glm::vec3& velocity)
	{
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it != m_Impl->Bodies.end())
			m_Impl->System->GetBodyInterface().SetLinearVelocity(it->second.ID, ToJolt(velocity));
	}

	glm::vec3 PhysicsWorld::GetLinearVelocity(Entity entity) const
	{
		auto it = m_Impl->Bodies.find(entity.GetUUID());
		if (it == m_Impl->Bodies.end())
			return glm::vec3(0.0f);
		return FromJolt(m_Impl->System->GetBodyInterface().GetLinearVelocity(it->second.ID));
	}

	void PhysicsWorld::SetAngularVelocity(Entity entity, const glm::vec3& velocity)
	{
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

	std::optional<RaycastHit> PhysicsWorld::Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, UUID ignoreEntity) const
	{
		const float length = glm::length(direction);
		if (length <= 0.0f || maxDistance <= 0.0f)
			return std::nullopt;
		const glm::vec3 unitDirection = direction / length;

		const JPH::RRayCast ray(ToJolt(origin), ToJolt(unitDirection * maxDistance));
		JPH::RayCastResult result;
		const RaycastBodyFilter bodyFilter(static_cast<uint64_t>(ignoreEntity));
		if (!m_Impl->System->GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, bodyFilter))
			return std::nullopt;

		RaycastHit hit;
		hit.Distance = result.mFraction * maxDistance;
		hit.Point = origin + unitDirection * hit.Distance;

		const JPH::BodyLockRead lock(m_Impl->System->GetBodyLockInterface(), result.mBodyID);
		if (lock.Succeeded())
		{
			const JPH::Body& body = lock.GetBody();
			hit.EntityID = body.GetUserData();
			hit.Normal = FromJolt(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, ToJolt(hit.Point)));
		}
		return hit;
	}

	glm::vec3 PhysicsWorld::GetGravity() const
	{
		return FromJolt(m_Impl->System->GetGravity());
	}

	void PhysicsWorld::SetGravity(const glm::vec3& gravity)
	{
		m_Impl->System->SetGravity(ToJolt(gravity));
	}

	uint32_t PhysicsWorld::GetBodyCount() const
	{
		return static_cast<uint32_t>(m_Impl->Bodies.size());
	}

}
