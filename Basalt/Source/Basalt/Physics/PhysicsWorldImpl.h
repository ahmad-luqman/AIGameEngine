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
#include "Basalt/Animation/FadingWeight.h"
#include "Basalt/Scene/Components.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/SimShapeFilter.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <limits>
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <span>
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
			// Pose last written to / read from the entity, used to detect transforms changed by scripts.
			glm::vec3 LastPosition = { 0.0f, 0.0f, 0.0f };
			glm::quat LastRotation = { 1.0f, 0.0f, 0.0f, 0.0f };
			// The world scale the colliders were built with (see ApplyScaleChanges).
			glm::vec3 BuiltScale = { 1.0f, 1.0f, 1.0f };
			// What the body was built from (plus later in-place updates), so what needs a rebuild says what the Jolt body is,
			// whatever the component says until the next ApplyPendingChanges. The warnings about its shape are added back by an
			// update that keeps it, to keep the logged set the same.
			RigidBodyComponent Settings;
			PhysicsInternal::ColliderSettings Colliders;
			std::vector<std::string> ShapeWarnings;
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
			// Whether the joint moves or connects to the entity's body.
			bool Touches(UUID uuid) const { return Body == uuid || Settings.ConnectedEntity == uuid; }
			// The second body's joint frame seen from the first's when the joint was built. Six-DOF frames
			// coincide there (identity), but Jolt picks each cone frame's Y and Z per body, so they can differ
			// by a twist; GetJointRotation measures from this.
			JPH::Quat RestRotation = JPH::Quat::sIdentity();
		};

		// Absent from Ragdolls: animation owns the rig. A live Doll owns simulation; a null Doll with
		// RecoveryLeft > 0 owns a frozen recovery pose. No Jolt handles are stored in authored components.
		struct RagdollRecord
		{
			JPH::Ref<JPH::Ragdoll> Doll;
			Ref<MeshSource> Source;
			// The settings the rig was built from, compared through RagdollNeedsRebuild (structural settings only). Live values
			// are read from the component.
			RagdollComponent Settings;
			// Parallel arrays in Jolt part order; part 0 is the selected skeleton root. Scales are world scales,
			// Capsules are BODY-local hemisphere centers/radii, and Nodes index Source's immutable hierarchy.
			std::vector<uint32_t> Nodes;
			std::vector<float> Scales;
			std::vector<RagdollCapsule> Capsules;
			// Authoritative world transforms only during recovery, including each joint's built uniform scale.
			std::vector<glm::mat4> FrozenWorld;
			// Last entity world transform, for moving a live or frozen rig when gameplay teleports its entity.
			glm::mat4 World = glm::mat4(1);
			float RecoveryLeft = 0;
			float RecoveryDuration = 0;
			// For each entry of Settings.Bones, the part it names. Read against live Bones only where the joint still matches.
			std::vector<int> BoneParts;
			// BalanceRagdoll's settings for one frame, in the terms it uses.
			struct BalanceDrive
			{
				float Strength = 0;
				// The spring's stiffness (1/s²) and damping (1/s).
				float Stiffness = 0;
				float Damping = 0;
				float CosMaxTilt = 1;
				// The parts' mass-weighted height above the lowest part's origin in the animated pose.
				float StandHeight = 0;
			};
			// What only a rig with live bodies has: reset with them (StopRagdollBodies).
			struct LiveState
			{
				// Whether the joint motors are on (DriveRagdollMotors). The motors stay untouched until strength * MotorTorque
				// first exceeds 0, and are turned off (false) when it drops to 0 or no usable animated pose exists.
				bool MotorsOn = false;
				// A KinematicRoot rig's character controller, on the entity or an ancestor (0: none), whose inner body and moves
				// the limbs stay out of. Followed every frame.
				UUID Character = 0;
				// Where the animation puts the root in model space, read once per physics frame; empty while it cannot say.
				std::optional<glm::mat4> AnimatedRoot;
				// Where the root was last sent in model space, so a frame with several steps divides the animated move among them.
				std::optional<glm::mat4> FollowedRoot;
				// This frame's balance; empty while balance is off.
				std::optional<BalanceDrive> Balance;
				// Whether the last step's balance held the rig up.
				bool Balanced = false;
			};
			LiveState Live;
			// A KinematicRoot rig leaves the entity's body or character in place and root motion to the animation.
			bool OwnsRoot() const { return !Settings.KinematicRoot; }
			bool HasPart(JPH::BodyID body) const { return Doll && std::ranges::find(Doll->GetBodyIDs(), body) != Doll->GetBodyIDs().end(); }
			float Weight() const { return Doll ? 1.0f : (RecoveryDuration > 0 ? RecoveryLeft / RecoveryDuration : 0.0f); }
		};
		// A rebuild carries physical orientation and COM velocities by joint name; new joints follow their
		// parent's physical orientation with the new model's bone offsets. Root position never snaps to animation.
		struct RagdollMotion
		{
			std::string Joint;
			glm::vec3 Position = glm::vec3(0);
			glm::vec3 Center = glm::vec3(0);
			glm::quat Rotation = glm::quat(1, 0, 0, 0);
			glm::vec3 Linear = glm::vec3(0);
			glm::vec3 Angular = glm::vec3(0);
		};
		// Structural failures wait for changed settings or source identity before automatic retries.
		struct RagdollFailure
		{
			RagdollComponent Settings;
			std::weak_ptr<MeshSource> Source;
			bool DependsOnSource = false;
		};
		PhysicsInternal::WarningLog RagdollWarnings{ "ragdoll on", false };
		// Motor problems (settings C++ wrote past validation, an animated pose that is not available) and failed
		// SetRagdollMotorStrength calls, each logged when they change.
		PhysicsInternal::WarningLog MotorWarnings{ "ragdoll motors on", true };
		PhysicsInternal::WarningLog MotorCallWarnings{ "ragdoll on", false };
		// Motor strength per Ragdoll entity: gameplay state that survives activation, recovery and rebuilds and ends with the
		// component (configured from MotorStrength, faded by SetRagdollMotorStrength).
		// No default constructor, so RagdollStrength[id] cannot compile: a default FadingWeight is 1, full torque.
		struct MotorStrength
		{
			explicit MotorStrength(float configured)
				: Weight(configured)
			{
			}
			FadingWeight Weight;
		};
		std::map<UUID, MotorStrength> RagdollStrength;
		void SyncRagdolls(PhysicsWorld& world, float seconds);
		// Once per fixed step, before Jolt's: root bodies follow the animation (the character has moved by now) and balance
		// forces, which Jolt clears after each step, are applied. `stepsLeft` counts this step and the frame's remaining ones.
		void StepRagdolls(float fixedStep, uint32_t stepsLeft);
		void FollowRagdollRoot(Entity entity, RagdollRecord& record, float fixedStep, uint32_t stepsLeft);
		void BalanceRagdoll(Entity entity, RagdollRecord& record, float fixedStep);
		// The velocity (at the part) of a body holding the rig up: touched by a part below `center` along `up`, by a surface
		// that faces up, and no trigger. Empty when nothing does.
		std::optional<JPH::Vec3> FindRagdollSupport(UUID uuid, const RagdollRecord& record, JPH::RVec3Arg center, JPH::Vec3Arg up) const;
		// Whether the entity's rig took over from its body or character (a KinematicRoot rig leaves them in place).
		bool RagdollReplacesBody(UUID uuid) const;
		// The character controller a rig on `entity` belongs to: on it or the nearest ancestor with one.
		UUID FindRagdollCharacter(Entity entity) const;
		// Whether `body` is a part of a rig belonging to that character.
		bool IsRagdollPartOf(const JPH::Body& body, UUID character) const;
		// Advances motor strengths, turns every active rig's joint motors toward its animated pose (or off at strength 0),
		// and records the AnimatedRoot and Balance that this frame's fixed steps use (a rig without motors that follows a root
		// still gets its AnimatedRoot).
		void DriveRagdollMotors(float seconds);
		void ApplyRagdollMotors(Entity entity, RagdollRecord& record, const RagdollComponent& component, float strength);
		// The entity's motor strength, created from (and configured by) its MotorStrength.
		FadingWeight& MotorStrengthFor(UUID uuid, const RagdollComponent& component);
		bool BuildRagdoll(Entity entity, RagdollRecord& record, std::string& error, bool& cacheFailure);
		bool ActivateRagdoll(PhysicsWorld& world, Entity entity, const glm::vec3& impulse, const std::vector<RagdollMotion>* motion = nullptr);
		void RemoveRagdoll(UUID uuid, bool deferContacts = false);
		void StopRagdollBodies(UUID uuid, RagdollRecord& record, bool deferContacts = false);
		bool ReadRagdollWorld(const RagdollRecord& record, std::vector<glm::mat4>& matrices) const;
		std::vector<RagdollMotion> CaptureRagdollMotion(const RagdollRecord& record) const;
		void SeedRagdoll(RagdollRecord& record, const std::vector<RagdollMotion>& motion);
		void DisableRagdoll(UUID uuid, const char* reason);
		void ValidateRagdollOutput();
		void OnRagdollDestroyed(entt::registry& registry, entt::entity handle);

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
			PhysicsInternal::ColliderSettings Colliders;
			// A RigidBody on the entity is ignored, which only a rebuild reports.
			bool IgnoresRigidBody = false;
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
			// As BodyRecord::BuiltScale.
			glm::vec3 BuiltScale = { 1.0f, 1.0f, 1.0f };
		};

		PhysicsCapacity Capacity;
		// The EPhysicsUpdateError bits already logged.
		uint32_t ReportedUpdateErrors = 0;
		// Logs what a step's System->Update dropped, which would otherwise show only as bodies falling through floors.
		void ReportUpdateErrors(JPH::EPhysicsUpdateError errors);

		Scope<JPH::TempAllocatorImplWithMallocFallback> TempAllocator;
		PhysicsJobs* JobSystem = nullptr; // Engine-owned; queued references may outlive this world.
		PhysicsInternal::BroadPhaseLayerInterfaceImpl BroadPhaseLayerInterface;
		PhysicsInternal::ObjectVsBroadPhaseLayerFilterImpl ObjectVsBroadPhaseLayerFilter;
		PhysicsInternal::ObjectLayerPairFilterImpl ObjectLayerPairFilter;
		PhysicsInternal::ContactListenerImpl ContactListener;
		Scope<JPH::PhysicsSystem> System;
		// Declared after System so records cannot outlive it, even during exceptional construction cleanup.
		std::map<UUID, RagdollRecord> Ragdolls;
		std::unordered_map<UUID, RagdollFailure> RagdollFailures;
		// Blocks reactivation while a callback tears down the entity or an unsupported rig.
		std::unordered_set<UUID> UnavailableRagdolls;
		// Scene shutdown must not dispatch gameplay contact exits.
		bool ShuttingDown = false;
		// Body/group allocation order is observable in deterministic replays.
		uint32_t NextRagdollGroup = 1;
		// Counts attempts, including rejected construction, for retry diagnostics.
		uint64_t RagdollBuilds = 0;

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

		// Character moves skip characters' inner bodies, as characters meet each other through CharacterCollision, and the
		// moving character's own rig, whose limbs hang inside its shape.
		class NonCharacterBodyFilter final : public JPH::BodyFilter
		{
		public:
			// `character`: the one moving, never 0.
			NonCharacterBodyFilter(const Impl& owner, UUID character)
				: m_Owner(owner)
				, m_Character(character)
			{
			}
			bool ShouldCollideLocked(const JPH::Body& body) const override { return !m_Owner.IsCharacterBody(body) && !m_Owner.IsRagdollPartOf(body, m_Character); }

		private:
			const Impl& m_Owner;
			UUID m_Character = 0;
		};

		// Keeps pairs of bodies from colliding. A shape filter, not a collide callback: Jolt's continuous collision job honors
		// the one and never calls the other. Runs on Jolt's worker threads, reading what only changes between steps.
		class BodyPairFilter final : public JPH::SimShapeFilter
		{
		public:
			explicit BodyPairFilter(const std::set<std::pair<uint64_t, uint64_t>>& ignoredPairs)
				: m_IgnoredPairs(ignoredPairs)
			{
			}
			bool ShouldCollide(const JPH::Body& body1, const JPH::Shape*, const JPH::SubShapeID&, const JPH::Body& body2, const JPH::Shape*, const JPH::SubShapeID&) const override;

		private:
			const std::set<std::pair<uint64_t, uint64_t>>& m_IgnoredPairs;
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
		BodyPairFilter BodyPairs{ IgnoredPairs };
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
		// Rebuilds bodies and characters whose components changed in a way only that can apply, updates the others in
		// place, applies changed character settings, resizes rescaled ones and rebuilds dirty joints (the start of every
		// Step, and OnSceneLoaded).
		void ApplyPendingChanges();
		// Applies a dirty entity's changed RigidBody and collider settings to its body or character, keeping contacts and
		// joints; values that did not change do nothing, not even wake it. False when only a rebuild can apply them.
		bool UpdateInPlace(Entity entity);
		bool UpdateBody(Entity entity, BodyRecord& record, const PhysicsInternal::ColliderSettings& colliders);
		bool UpdateCharacter(Entity entity, CharacterRecord& record, const PhysicsInternal::ColliderSettings& colliders);
		bool ReplaceBodyColliders(Entity entity, BodyRecord& record, const PhysicsInternal::ColliderSettings& colliders);
		// Gives the body the new shape and its materials and (when dynamic) inertia at the component's mass. Jolt keeps the
		// body's origin and re-evaluates its contacts without ending them. Its center of mass can move, which joints must be
		// told of (or be gone). False as ApplyBodyMass is.
		bool SwapBodyShape(Entity entity, BodyRecord& record, const PhysicsInternal::ColliderShape& colliders);
		// The body's inertia from its shape at the mass. False, logged, when the body cannot be locked: only a rebuild can
		// apply it then, so callers rebuild.
		bool ApplyBodyMass(Entity entity, const BodyRecord& record, float mass);
		// Colliders are sized by the entity's world scale, which a script, an animation or a parent can change
		// without touching a physics component. Once per Step (and on a scene load), after dirty entities are
		// applied, bodies and characters whose scale differs from the one they were built with get a new shape in
		// place, keeping their contacts (so an animated scale does not end and restart every overlap each frame);
		// a body's joints are rebuilt from the current poses, since their anchors scale with it.
		// Entities left without a body by a degenerate transform are built once it decomposes again.
		void ApplyScaleChanges();
		void ResizeBody(Entity entity, const glm::vec3& scale);
		void ResizeCharacter(Entity entity, const glm::vec3& scale);
		// Gives the character new shapes for the scale; false with nothing changed when they cannot be built.
		bool ReshapeCharacter(Entity entity, CharacterRecord& record, const glm::vec3& scale, std::vector<std::string>& warnings);
		// What the character touches now, for a step that must not start from what it touched before a new shape.
		void RefreshCharacterContacts(Entity entity);
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

		// Ends the contacts of bodies an entity lost (it may keep others) and notifies the surviving entities of the pairs
		// that no longer touch at all.
		void EndContacts(UUID uuid, std::span<const JPH::BodyID> bodies, bool trigger, bool defer = false);
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
