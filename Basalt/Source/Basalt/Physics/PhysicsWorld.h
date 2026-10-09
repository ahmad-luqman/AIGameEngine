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

	// Jolt-backed physics simulation for one running scene.
	//
	// Bodies are created for every entity with a RigidBodyComponent and at least one collider. Entities
	// whose physics components are added or removed at runtime are rebuilt before the next step.
	// JointComponents become Jolt constraints once both bodies exist (a joint whose body is missing is
	// retried when that body is created). A joint is rebuilt from the current poses when either body is
	// rebuilt or one of its structural fields changes (see JointComponent); other changes update it in place.
	// Joint warnings are logged once per distinct setting, so scripts may set the component every frame.
	// Collision filtering uses named layers (RigidBodyComponent::Layer) and the collision matrix of the
	// scene's override or else the active project, copied when the world is built: matrix edits apply on the
	// next play. An unknown layer name falls back to Default with a warning.
	// Dynamic bodies write their pose back to the entity; moving a dynamic or static body's transform from
	// script teleports it; kinematic bodies follow their transform smoothly.
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
		glm::vec3 GetLinearVelocity(Entity entity) const;
		void SetAngularVelocity(Entity entity, const glm::vec3& velocity);
		glm::vec3 GetAngularVelocity(Entity entity) const;

		// Whether the entity's JointComponent is currently a live constraint (false before both bodies
		// exist, when the joint is invalid, and after it broke).
		bool HasJoint(Entity entity) const;
		// Hinge angle (degrees, wrapping at +-180) or slider offset (meters) of this entity relative to the
		// connected one, measured from the joint's rest pose and signed around/along the world-space Axis;
		// nullopt for other joint types and entities without a live joint.
		std::optional<float> GetJointPosition(Entity entity) const;

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

	private:
		void RebuildDirtyJoints();
		// Marks every joint owned by or connected to the entity for a rebuild (its body was just created).
		void MarkJointsDirty(UUID uuid);
		// Builds the entity's constraint; problems are appended to warnings.
		void CreateJoint(Entity entity, std::vector<std::string>& warnings);
		// Applies the fields of a live joint that do not need a rebuild (limits, motor, break thresholds,
		// collision).
		void ApplyJointSettings(Entity entity, std::vector<std::string>& warnings);
		// Logs the warnings unless the same ones were already logged for the same settings.
		void ReportJointWarnings(Entity entity, std::vector<std::string> warnings);
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
