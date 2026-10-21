#pragma once

// Internal to the physics module: PhysicsWorld's state, shared by its source files (PhysicsWorld.cpp,
// PhysicsJoints.cpp, PhysicsCharacters.cpp, PhysicsContacts.cpp, PhysicsQueries.cpp).

#include "Basalt/Physics/PhysicsWorld.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Physics/ContactListener.h"
#include "Basalt/Physics/DirtySet.h"
#include "Basalt/Physics/JoltUtils.h"
#include "Basalt/Physics/PhysicsJobs.h"
#include "Basalt/Physics/PhysicsLayers.h"
#include "Basalt/Physics/WarningLog.h"
#include "Basalt/Scene/Components.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Basalt {

	struct PhysicsWorld::Impl
	{
		explicit Impl(Scene* scene);
		~Impl();

		// The owning scene outlives this world; one world owns the registry's runtime records.
		Scene* const ScenePtr;

		// Runtime-only EnTT component; never copied or serialized with scene data.
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
			// The world scale the colliders were built with (see ApplyScaleChanges).
			glm::vec3 BuiltScale = { 1.0f, 1.0f, 1.0f };
			// Set while the entity's transform cannot be decomposed (e.g. a zero scale): the body keeps its shape
			// and simulates on, but its pose is not written back (warned once per episode).
			bool Degenerate = false;
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

		struct RagdollRecord
		{
			JPH::Ref<JPH::Ragdoll> Doll;
			Ref<MeshSource> Source;
			RagdollComponent Settings;
			std::vector<uint32_t> Nodes;
			std::vector<float> Scales;
			std::vector<RagdollCapsule> Capsules;
			std::vector<glm::mat4> FrozenWorld;
			glm::vec3 InitialRoot = glm::vec3(0);
			glm::mat4 World = glm::mat4(1);
			float RecoveryLeft = 0;
			float RecoveryDuration = 0;
		};
		std::map<UUID, RagdollRecord> Ragdolls;
		uint32_t NextRagdollGroup = 1;
		PhysicsInternal::WarningLog RagdollWarnings{ "ragdoll on", false };
		void SyncRagdolls(PhysicsWorld& world, float seconds);
		bool BuildRagdoll(Entity entity, RagdollRecord& record, std::string& error);
		void RemoveRagdoll(UUID uuid);
		void StopRagdollBodies(UUID uuid, RagdollRecord& record);
		void FreezeRagdoll(RagdollRecord& record);

		struct CharacterRecord
		{
			JPH::Ref<JPH::CharacterVirtual> Character;
			// The settings applied to the character (from its CharacterControllerComponent).
			CharacterControllerComponent Settings;
			uint32_t Layer = 0;
			// Settings in Jolt's terms, checked by ConfigureCharacter.
			float StepHeight = 0.0f;
			float GravityFactor = 1.0f;
			float JumpGrace = 0.0f;
			// Seconds since the character last stood on walkable ground (counted in fixed steps), and whether it jumped
			// since then (from the ground or within JumpGrace), so the grace gives one jump. A character that has not
			// stood yet (built in the air) gets none; a rebuild keeps both.
			struct Airtime
			{
				float SinceGrounded = std::numeric_limits<float>::infinity();
				bool Jumped = false;
			};
			Airtime Air;
			// Warnings found while building the character, repeated with the settings' own when they change.
			std::vector<std::string> BuildWarnings;
			// The last MoveCharacter velocity, used every step until the next call.
			glm::vec3 MoveVelocity = { 0.0f, 0.0f, 0.0f };
			// Animation movement for one Step, shared by its substeps: the root motion velocity and how much of the
			// horizontal movement it owns (see QueueRootMotion). Absent restores gameplay movement.
			struct RootMotionDrive
			{
				glm::vec3 Velocity = glm::vec3(0);
				float Share = 1.0f;
			};
			std::optional<RootMotionDrive> RootMotion;
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
		PhysicsJobs* JobSystem = nullptr; // Engine-owned; queued references may outlive this world.
		PhysicsInternal::BroadPhaseLayerInterfaceImpl BroadPhaseLayerInterface;
		PhysicsInternal::ObjectVsBroadPhaseLayerFilterImpl ObjectVsBroadPhaseLayerFilter;
		PhysicsInternal::ObjectLayerPairFilterImpl ObjectLayerPairFilter;
		PhysicsInternal::ContactListenerImpl ContactListener;
		Scope<JPH::PhysicsSystem> System;

		// Applies the collision matrix between characters, which CharacterCollision does not know about.
		class CharacterLayerFilter final : public JPH::CharacterContactListener
		{
		public:
			explicit CharacterLayerFilter(const Impl& owner)
				: m_Owner(owner)
			{
			}
			bool OnCharacterContactValidate(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact) override;

		private:
			const Impl& m_Owner;
		};

		// Character moves skip characters' inner bodies: characters meet each other through CharacterCollision.
		class NonCharacterBodyFilter final : public JPH::BodyFilter
		{
		public:
			explicit NonCharacterBodyFilter(const Impl& owner)
				: m_Owner(owner)
			{
			}
			bool ShouldCollideLocked(const JPH::Body& body) const override { return !m_Owner.IsCharacterBody(body); }

		private:
			const Impl& m_Owner;
		};

		std::unordered_map<UUID, CharacterRecord> Characters;
		// Requests live outside character records so creating or rebuilding a collider cannot lose motion.
		struct RootMotionRequest
		{
			glm::vec3 Pending = glm::vec3(0);
			// The shares queued since the last step, weighted by their frames' durations (one fixed step for a frame
			// queued without one), and the summed durations: a frame without displacement still queues its share.
			float ShareTime = 0;
			float Time = 0;
			bool Active = false;
			// Unbuilt entries and InvalidReported retain idle requests until construction/recovery,
			// keeping these diagnostic flags alive to prevent repeated logs.
			bool UnbuiltReported = false;
			bool InvalidReported = false;
			bool InvalidShareReported = false;

			// Every queued frame adds to Time.
			bool HasQueued() const { return Time > 0; }
			void ClearQueued()
			{
				Pending = glm::vec3(0);
				ShareTime = 0;
				Time = 0;
			}
		};
		std::unordered_map<UUID, RootMotionRequest> RootMotion;

		// Characters meet each other's full shapes through this list, and a moving one pushes the other with its
		// velocity (in the pushed character's own update). They skip each other's inner bodies, which Jolt
		// moves by teleporting, so those would only block. Holds raw pointers, one per Characters record:
		// RemoveCharacter takes a character out before its record goes, and the destructor clears it first.
		// Characters are in the order they were last built (a rebuilt one moves to the end), which is
		// deterministic but not the registry order.
		JPH::CharacterVsCharacterCollisionSimple CharacterCollision;
		CharacterLayerFilter CharacterLayers{ *this };
		NonCharacterBodyFilter CharacterBodyFilter{ *this };
		// Entities that got no body or character because their transform could not be decomposed (e.g. spawned
		// at a zero scale); ApplyScaleChanges builds them once it can. A live body or character whose transform
		// turns degenerate keeps its record instead (BodyRecord/CharacterRecord::Degenerate).
		std::unordered_set<UUID> Unbuilt;
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
		// Shape cooking can fail at a new scale even when the collider components are unchanged.
		PhysicsInternal::WarningLog ResizeWarnings{ "resized entity", true };
		// Set while Start() creates every body; it marks all joints dirty itself afterwards.
		bool Starting = false;
		using EntityPair = std::pair<uint64_t, uint64_t>;
		// Entity pairs (lower UUID first) joined by a joint with EnableCollision off. Read by Jolt worker
		// threads during Update, so it only changes between steps.
		std::set<EntityPair> IgnoredPairs;
		struct ContactState;
		// Keep contact bookkeeping local to PhysicsContacts.cpp; Impl construction/destruction is defined there.
		Scope<ContactState> Contacts;

		// Rebuilds the entity's body, preserving velocity and ending contacts of the old shape.
		void RecreateBody(Entity entity);
		// Rebuilds (or removes) the entity's character; keeps its velocity and the last Move velocity.
		void RecreateCharacter(Entity entity);
		// Rebuilds bodies and characters whose components changed, applies changed character settings, resizes
		// rescaled ones and rebuilds dirty joints (the start of every Step, and OnSceneLoaded).
		void ApplyPendingChanges();
		// Colliders are sized by the entity's world scale, which a script, an animation or a parent can change
		// without touching a physics component. Once per Step (and on a scene load), after dirty entities are
		// rebuilt, bodies and characters whose scale differs from the one they were built with get a new shape in
		// place, keeping their contacts (so an animated scale does not end and restart every overlap each frame);
		// a body's joints are rebuilt from the current poses, since their anchors scale with it.
		// Entities left without a body by a degenerate transform are built once it decomposes again.
		void ApplyScaleChanges();
		void ResizeBody(Entity entity, const glm::vec3& scale);
		void ResizeCharacter(Entity entity, const glm::vec3& scale);
		// Applies changed CharacterController settings in place, rebuilding only when the layer changed.
		void ApplyCharacterSettings(Entity entity);
		void UpdateCharacters(float fixedStep);
		void RebuildDirtyJoints();
		// Marks every joint moving or connected to the entity's body for a rebuild (it was just created).
		void MarkJointsDirty(UUID uuid);
		// Builds the entity's constraint; problems are appended to warnings.
		void CreateJoint(Entity entity, std::vector<std::string>& warnings);
		// Applies the fields of a live joint that do not need a rebuild (limits, motor, break thresholds,
		// collision).
		void ApplyJointSettings(Entity entity, std::vector<std::string>& warnings);
		// Removes joints whose constraint force or torque exceeded their break thresholds in the last step.
		void CheckBrokenJoints(float fixedStep);
		void DispatchContacts(float fixedStep);

		bool Owns(Entity entity) const { return entity.GetScene() == ScenePtr && entity; }
		// Borrowed pointers must not survive mutations of the registry's BodyRecord pool.
		BodyRecord* FindBody(Entity entity) const;
		BodyRecord* FindBody(UUID uuid) const;
		// Whether a (possibly null) record is a dynamic or kinematic body Jolt has put to sleep; static bodies are always
		// inactive in Jolt, which is not sleep. Look records up by Entity where the scene matters: the play copy shares the
		// edit scene's UUIDs.
		bool IsSleeping(const BodyRecord* record) const;
		BodyRecord* MutableBody(Entity entity, const char* call);
		auto BodyView() const
		{
			auto view = ScenePtr->GetRegistry().view<RigidBodyComponent, BodyRecord>();
			// Rebuilds reorder BodyRecord storage; activation must follow the authored component order.
			view.use<RigidBodyComponent>();
			return view;
		}

		// Applies the record's Settings that a live character can change, sanitizing out-of-range values
		// (appended to warnings).
		static void ConfigureCharacter(CharacterRecord& record, std::vector<std::string>& warnings);

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
		void RemoveBody(UUID uuid);

		// Removes a character and its inner body, ending its contacts like RemoveBody.
		void RemoveCharacter(UUID uuid);

		// Ends every contact of an entity whose body is gone and notifies the surviving entities.
		void EndContacts(UUID uuid, bool trigger);
		// Reports one contact transition to the scene's event listener, then to the scripts.
		void DispatchContact(ContactEventType type, Entity first, Entity second, const std::optional<ContactInfo>& contact);

		// Where two entities whose contact just ended parted: the closest points of their bodies (see
		// ContactInfo), relative to the first. nullopt when either has no body or they are already far apart.
		std::optional<ContactInfo> SeparationContact(UUID first, UUID second, float fixedStep) const;

		// Writes a simulated pose to the entity, keeping its scale's magnitudes exactly: read back from the world
		// matrix they would pick up rounding every step and drift (enough after a few hundred steps to count as a
		// rescale). The signs come from the matrix, which cannot say which axis was mirrored; the decomposed
		// rotation assumes a negative X (see DecomposeTransform), so a Y- or Z-mirrored entity's Scale and
		// Rotation are re-expressed that way on its first write-back.
		// Returns false, writing nothing, while the entity's transform cannot be decomposed: a pose composed with
		// a zero scale (or under a zero-scaled parent) would lose its rotation or turn into NaN.
		static bool WritePose(Entity entity, const glm::vec3& position, const glm::quat& rotation);

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
