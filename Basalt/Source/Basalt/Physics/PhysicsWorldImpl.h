#pragma once

// Internal to the physics module: PhysicsWorld's state, shared by its source files (PhysicsWorld.cpp,
// PhysicsJoints.cpp, PhysicsCharacters.cpp, PhysicsQueries.cpp).

#include "Basalt/Physics/PhysicsWorld.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Physics/ContactListener.h"
#include "Basalt/Physics/DirtySet.h"
#include "Basalt/Physics/JoltUtils.h"
#include "Basalt/Physics/PhysicsLayers.h"
#include "Basalt/Physics/WarningLog.h"
#include "Basalt/Scene/Components.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Basalt {

	struct PhysicsWorld::Impl
	{
		struct BodyRecord
		{
			JPH::BodyID ID;
			RigidBodyType Type = RigidBodyType::Static;
			bool IsTrigger = false;
			// The MeshComponent mesh a MeshCollider without its own Mesh was built from, so edits to the
			// MeshComponent's other fields (e.g. CastShadows) do not rebuild the body.
			std::optional<std::pair<std::string, uint32_t>> BorrowedMesh;
			// Pose last written to / read from the entity, used to detect transforms changed by scripts.
			glm::vec3 LastPosition = { 0.0f, 0.0f, 0.0f };
			glm::quat LastRotation = { 1.0f, 0.0f, 0.0f, 0.0f };
			// The world scale the colliders were built with (see MarkRescaledDirty).
			glm::vec3 BuiltScale = { 1.0f, 1.0f, 1.0f };
		};

		struct JointRecord
		{
			JPH::Ref<JPH::TwoBodyConstraint> Constraint;
			// The component the constraint was built from (plus later in-place updates). Its Type always
			// matches the constraint's subtype; ConnectedEntity is 0 when attached to the world.
			JointComponent Settings;
			// The entity whose body the joint moves (Settings.BodyEntity resolved; the holder when that is 0).
			// Only set when the constraint is built; that stays correct because changing BodyEntity rebuilds
			// the joint (see JointNeedsRebuild).
			UUID Body = 0;
			// Warnings found while building the constraint (e.g. a zero axis). In-place updates only re-check
			// the other settings, so these are added back to keep the logged set the same.
			std::vector<std::string> BuildWarnings;
			// The second body's joint frame seen from the first's when the joint was built. Six-DOF frames
			// coincide there (identity), but Jolt picks each cone frame's Y and Z per body, so they can differ
			// by a twist; GetJointRotation measures from this.
			JPH::Quat RestRotation = JPH::Quat::sIdentity();
		};

		struct CharacterRecord
		{
			JPH::Ref<JPH::CharacterVirtual> Character;
			// The settings applied to the character (from its CharacterControllerComponent).
			CharacterControllerComponent Settings;
			uint32_t Layer = 0;
			// Settings in Jolt's terms, checked by ConfigureCharacter.
			float StepHeight = 0.0f;
			float GravityFactor = 1.0f;
			// Warnings found while building the character, repeated with the settings' own when they change.
			std::vector<std::string> BuildWarnings;
			// The last MoveCharacter velocity, used every step until the next call.
			glm::vec3 MoveVelocity = { 0.0f, 0.0f, 0.0f };
			// Displacement over the last step divided by its duration.
			glm::vec3 ActualVelocity = { 0.0f, 0.0f, 0.0f };
			// Pose last written to / read from the entity, used to detect transforms changed by scripts.
			glm::vec3 LastPosition = { 0.0f, 0.0f, 0.0f };
			glm::quat LastRotation = { 1.0f, 0.0f, 0.0f, 0.0f };
			// The shape's bounding box in the character's local space, relative to its position.
			JPH::AABox Bounds;
			// Set while the entity's transform cannot be decomposed (e.g. a zero scale); the character waits.
			bool Degenerate = false;
			// As BodyRecord::BorrowedMesh and BuiltScale.
			std::optional<std::pair<std::string, uint32_t>> BorrowedMesh;
			glm::vec3 BuiltScale = { 1.0f, 1.0f, 1.0f };
		};

		Scope<JPH::TempAllocatorImpl> TempAllocator;
		Scope<JPH::JobSystemThreadPool> JobSystem;
		PhysicsInternal::BroadPhaseLayerInterfaceImpl BroadPhaseLayerInterface;
		PhysicsInternal::ObjectVsBroadPhaseLayerFilterImpl ObjectVsBroadPhaseLayerFilter;
		PhysicsInternal::ObjectLayerPairFilterImpl ObjectLayerPairFilter;
		PhysicsInternal::ContactListenerImpl ContactListener;
		Scope<JPH::PhysicsSystem> System;

		// Applies the collision matrix between characters, which CharacterCollision does not know about.
		struct CharacterLayerFilter final : JPH::CharacterContactListener
		{
			const Impl* Owner = nullptr;
			bool OnCharacterContactValidate(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact) override;
		};

		std::unordered_map<UUID, BodyRecord> Bodies;
		std::unordered_map<UUID, CharacterRecord> Characters;
		// Characters meet each other's full shapes through this list, and a moving one pushes the other with its
		// velocity (in the pushed character's own update). They skip each other's inner bodies, which Jolt
		// moves by teleporting, so those would only block. Holds raw pointers: RemoveCharacter takes a
		// character out before its record goes, in creation order otherwise.
		JPH::CharacterVsCharacterCollisionSimple CharacterCollision;
		CharacterLayerFilter CharacterListener;
		// Characters whose CharacterControllerComponent changed (collider changes go to DirtyEntities).
		PhysicsInternal::DirtySet DirtyCharacters;
		// Character warnings (sanitized settings, an ignored RigidBody, a missing collider).
		PhysicsInternal::WarningLog CharacterWarnings{ "character", true };
		// Characters already warned that rigid-body velocity and force calls do nothing on them.
		std::unordered_set<UUID> WarnedCharacterBodyCalls;
		std::unordered_map<uint32_t, UUID> BodyToEntity;
		PhysicsInternal::DirtySet DirtyEntities;
		// Keyed by the entity that holds the JointComponent (not necessarily the body it moves).
		std::unordered_map<UUID, JointRecord> Joints;
		// A set, not a DirtySet: RebuildDirtyJoints walks joints in registry order and only asks for membership.
		std::unordered_set<UUID> DirtyJoints;
		// Joint warnings, one line each (fields the joint ignores, clamped limits, why it is not built).
		PhysicsInternal::WarningLog JointWarnings{ "joint on", false };
		// The collision layers this world was built with (scene override, else the active project's).
		PhysicsLayers Layers;
		// Body warnings (e.g. an unknown layer or an ignored Continuous).
		PhysicsInternal::WarningLog BodyWarnings{ "entity", true };
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

		// Applies the record's Settings that a live character can change, sanitizing out-of-range values
		// (reported through warn).
		static void ConfigureCharacter(CharacterRecord& record, const std::function<void(const std::string&)>& warn);

		// Only contacts on the lower part of the shape can be ground; others are walls or ceilings. "Lower" is
		// along the character's up axis (against gravity) in its local space, so a turned gravity or a tilted
		// entity keeps the bottom of the shape as its feet: below the lowest point of its bounds plus their
		// narrowest half extent (the hemisphere of an upright capsule).
		static void UpdateSupportingVolume(CharacterRecord& record);

		// Velocity and force calls address rigid bodies; a character moves only through Move. Warns once per
		// character so knockback code that silently does nothing is noticed. Returns whether it is a character.
		bool WarnIfCharacter(Entity entity, const char* call);

		bool IsTrigger(UUID uuid) const;

		// Whether the body is a character's inner body (see CharacterCollision).
		bool IsCharacterBody(const JPH::Body& body) const;

		void UpdateIgnoredPairs();

		void RemoveJoint(UUID holder);

		// Jolt constraints point at their bodies, so they must go before either body is destroyed. They are
		// rebuilt before the next step if both bodies still exist then (e.g. a body that was recreated).
		void RemoveJointsOfBody(UUID uuid);

		// Removes a body. Contacts it had are ended now (Jolt's removal callbacks for a destroyed body can
		// no longer be mapped to an entity), and surviving entities are notified.
		void RemoveBody(Scene* scene, UUID uuid);

		// Removes a character and its inner body, ending its contacts like RemoveBody.
		void RemoveCharacter(Scene* scene, UUID uuid);

		// Ends every contact of an entity whose body is gone and notifies the surviving entities.
		void EndContacts(Scene* scene, UUID uuid, bool trigger);

		// Where two entities whose contact just ended parted: the closest points of their bodies (see
		// ContactInfo), relative to the first. nullopt when either has no body or they are already far apart.
		std::optional<ContactInfo> SeparationContact(UUID first, UUID second, float fixedStep) const;

		// Colliders are sized by the entity's world scale, which a script, an animation or a parent can change
		// without touching a physics component: marks bodies and characters whose scale differs from the one
		// they were built with for a rebuild. Runs once per Step, in registry order, before dirty entities are
		// rebuilt, so a rebuilt body's joints are back before the next fixed step.
		void MarkRescaledDirty(Scene* scene);

		void OnPhysicsComponentChanged(entt::registry& registry, entt::entity entity);

		// A MeshCollider without its own Mesh collides by the entity's MeshComponent.
		void OnMeshChanged(entt::registry& registry, entt::entity entity);

		// Only a different mesh changes the collider.
		void OnMeshUpdated(entt::registry& registry, entt::entity entity);

		void OnCharacterChanged(entt::registry& registry, entt::entity entity);

		void OnJointChanged(entt::registry& registry, entt::entity entity);

		void OnJointDestroyed(entt::registry& registry, entt::entity entity);

		// Connects (or disconnects) every component signal the world listens to, from one list so the
		// constructor and destructor cannot drift apart.
		void WireSignals(entt::registry& registry, bool connect);

		template<typename Component, auto OnConstruct, auto OnUpdate, auto OnDestroy>
		void Wire(entt::registry& registry, bool connect)
		{
			if (connect)
			{
				registry.on_construct<Component>().template connect<OnConstruct>(*this);
				registry.on_update<Component>().template connect<OnUpdate>(*this);
				registry.on_destroy<Component>().template connect<OnDestroy>(*this);
			}
			else
			{
				registry.on_construct<Component>().template disconnect<OnConstruct>(*this);
				registry.on_update<Component>().template disconnect<OnUpdate>(*this);
				registry.on_destroy<Component>().template disconnect<OnDestroy>(*this);
			}
		}
	};

}
