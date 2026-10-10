#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Core/Timestep.h"
#include "Basalt/Core/UUID.h"
#include "Basalt/Physics/PhysicsLayers.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	class Entity;
	class Scene;

	struct RaycastHit
	{
		UUID EntityID = 0;
		glm::vec3 Point = { 0.0f, 0.0f, 0.0f };
		glm::vec3 Normal = { 0.0f, 0.0f, 0.0f };
		float Distance = 0.0f;
	};

	// Which bodies a query sees. LayerMask has one bit per physics layer (see PhysicsLayers::MaskFromNames);
	// a mask of 0 matches nothing.
	struct PhysicsQueryFilter
	{
		uint32_t LayerMask = 0xFFFFFFFF;
		// Skipped entirely, e.g. the caster itself; 0 (never a valid entity UUID) skips nothing.
		UUID IgnoreEntity = 0;
		bool IncludeTriggers = false;
	};

	enum class ContactEventType
	{
		CollisionBegin = 0,
		CollisionEnd,
		TriggerEnter,
		TriggerExit
	};

	// Where and how hard two entities started touching (ContactEventType::CollisionBegin). Relative to the
	// event's first entity: Normal points from the second entity toward the first, i.e. the direction that
	// pushes the first entity out.
	struct ContactInfo
	{
		glm::vec3 Point = { 0.0f, 0.0f, 0.0f };
		glm::vec3 Normal = { 0.0f, 0.0f, 0.0f };
		// Estimated impulse (N*s) along the normal that resolves the impact, bounce included; 0 when the
		// bodies touched without closing in (e.g. resting contact). The strongest touching part is reported
		// when several parts of the two bodies touch in the same step.
		float Impulse = 0.0f;

		// The same contact seen from the second entity.
		ContactInfo Flipped() const { return { Point, -Normal, Impulse }; }
	};

	// Jolt-backed physics simulation for one running scene.
	//
	// Bodies are created for every entity with a RigidBodyComponent and at least one collider (box, sphere,
	// capsule or mesh; several combine into one compound shape). Entities whose physics components are added,
	// changed or removed at runtime are rebuilt before the next step, and so are bodies and characters whose
	// world scale changed (colliders are sized by it).
	// JointComponents become Jolt constraints once both bodies exist (a joint whose body is missing is
	// retried when that body is created). A joint moves its BodyEntity's body, so several joint entities can
	// act on one body; joints are tracked by the entity holding the component. A joint is rebuilt from the
	// current poses when either body is rebuilt or one of its structural fields changes (see JointComponent);
	// other changes update it in place. Joint warnings (including fields the joint ignores, see
	// JointFieldApplies) are logged once per distinct setting, so scripts may set the component every frame.
	// Contacts combine the two bodies' Friction and Restitution with their FrictionCombine/RestitutionCombine
	// modes (see CombineFriction in PhysicsMaterial.h).
	// Collision filtering uses named layers (RigidBodyComponent::Layer) and the collision matrix of the
	// scene's override or else the active project, copied when the world is built: matrix edits apply on the
	// next play. An unknown layer name falls back to Default with a warning.
	// Dynamic bodies write their pose back to the entity; moving a dynamic or static body's transform from
	// script teleports it; kinematic bodies follow their transform smoothly.
	// Entities with a CharacterControllerComponent and a collider get a Jolt CharacterVirtual (plus a kinematic
	// inner body that other bodies, queries and contact events see) instead of a rigid body. Characters move
	// before each step, in registry order, and write their position back; their rotation follows the entity.
	class PhysicsWorld
	{
	public:
		explicit PhysicsWorld(Scene* scene);
		~PhysicsWorld();

		PhysicsWorld(const PhysicsWorld&) = delete;
		PhysicsWorld& operator=(const PhysicsWorld&) = delete;

		void Start();
		// Runs as many fixed steps as the accumulated time allows, then dispatches contact events.
		void Step(Timestep ts);
		void OnEntityDestroyed(Entity entity);

		bool HasBody(Entity entity) const;
		// Rebuilds the body from the entity's current components (e.g. after changing collider sizes).
		void RecreateBody(Entity entity);

		void AddForce(Entity entity, const glm::vec3& force);
		void AddImpulse(Entity entity, const glm::vec3& impulse);
		void AddTorque(Entity entity, const glm::vec3& torque);
		void SetLinearVelocity(Entity entity, const glm::vec3& velocity);
		// For a character, how fast it actually moved in the last step (after collisions, steps and slopes).
		glm::vec3 GetLinearVelocity(Entity entity) const;
		void SetAngularVelocity(Entity entity, const glm::vec3& velocity);
		glm::vec3 GetAngularVelocity(Entity entity) const;

		// Whether the JointComponent held by this entity is currently a live constraint (false before both
		// bodies exist, when the joint is invalid, and after it broke).
		bool HasJoint(Entity entity) const;
		// Hinge angle (degrees, wrapping at +-180) or slider offset (meters) of the joint held by this entity:
		// its body relative to the connected one, measured from the joint's rest pose and signed around/along
		// the world-space Axis; nullopt for other joint types and entities without a live joint.
		std::optional<float> GetJointPosition(Entity entity) const;
		// Rotation of a cone or six-DOF joint's body relative to the connected one, measured from the joint's
		// rest pose in the joint frame (X = Axis; for six-DOF Y = SecondaryAxis, for a cone Y and Z are an
		// arbitrary perpendicular pair), as Euler angles in degrees like Transform Rotation. A six-DOF
		// AngularMotorTarget in this form drives the joint back to that pose. nullopt for other joint types
		// and entities without a live joint.
		std::optional<glm::vec3> GetJointRotation(Entity entity) const;

		// Character controllers. MoveCharacter sets the velocity the character walks with until the next call.
		// With gravity (scene gravity times GravityFactor), the part of it along the up axis (against gravity)
		// only counts while the character stands on walkable ground, where it jumps (again on every landing
		// while it is held); in the air gravity drives the vertical speed and the rest steers. The character
		// also moves with the ground it stands on. Without gravity the velocity applies in full. Calls on
		// entities without a character do nothing; non-finite velocities are ignored.
		bool HasCharacter(Entity entity) const;
		void MoveCharacter(Entity entity, const glm::vec3& velocity);
		// Standing on ground no steeper than SlopeLimit.
		bool IsCharacterGrounded(Entity entity) const;
		// Normal of the ground the character touches (walkable or too steep), nullopt in the air or without a
		// character.
		std::optional<glm::vec3> GetCharacterGroundNormal(Entity entity) const;
		uint32_t GetCharacterCount() const;

		// Scene queries. Casts sweep along `direction` (any length) for up to maxDistance and report the
		// closest hit, or with the *All variants every entity hit, once each at its closest point, sorted by
		// distance (ties by UUID). A cast that starts inside a body hits it at distance 0, whichever way it
		// moves. Overlaps return each entity touching the shape once, sorted by UUID. Invalid input (a
		// non-finite or zero-length direction, a non-finite position, a non-positive distance, radius or half
		// extent, a zero rotation) returns nothing. Rotations are world-space; box sizes are half extents.
		std::optional<RaycastHit> Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter = {}) const;
		std::vector<RaycastHit> RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter = {}) const;
		std::optional<RaycastHit> SphereCast(const glm::vec3& origin, float radius, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter = {}) const;
		std::vector<RaycastHit> SphereCastAll(const glm::vec3& origin, float radius, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter = {}) const;
		std::optional<RaycastHit> BoxCast(const glm::vec3& origin, const glm::vec3& halfExtents, const glm::quat& rotation, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter = {}) const;
		std::vector<RaycastHit> BoxCastAll(const glm::vec3& origin, const glm::vec3& halfExtents, const glm::quat& rotation, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter = {}) const;
		std::vector<UUID> OverlapSphere(const glm::vec3& center, float radius, const PhysicsQueryFilter& filter = {}) const;
		std::vector<UUID> OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, const PhysicsQueryFilter& filter = {}) const;

		// The layers this world filters with (resolved when it was built).
		const PhysicsLayers& GetLayers() const;

		glm::vec3 GetGravity() const;
		void SetGravity(const glm::vec3& gravity);

		uint32_t GetBodyCount() const;
		uint64_t GetStepCount() const { return m_StepCount; }
		// How many mesh collider shapes have been cooked by every world so far. Cooked shapes are cached per
		// mesh asset, mesh index and convexity across worlds, so this only grows for new or reloaded meshes.
		static uint64_t GetMeshShapeCookCount();
		// Drops every cached mesh collider shape (e.g. after AssetManager::Clear on a project switch). Shapes
		// still used by a body stay alive through that body.
		static void ClearMeshShapeCache();

	private:
		// Rebuilds (or removes) the entity's character; keeps its velocity and the last Move velocity.
		void RecreateCharacter(Entity entity);
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
		void DispatchContacts();

	private:
		struct Impl;
		Scope<Impl> m_Impl;
		Scene* m_Scene = nullptr;
		float m_Accumulator = 0.0f;
		uint64_t m_StepCount = 0;
	};

}
