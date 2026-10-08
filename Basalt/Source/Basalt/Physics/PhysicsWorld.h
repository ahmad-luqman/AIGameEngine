#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Core/Timestep.h"
#include "Basalt/Core/UUID.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>

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
	// JointComponents become Jolt constraints once both bodies exist; a joint is rebuilt (from the current
	// poses) whenever it changes or either of its bodies is rebuilt.
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

		bool HasJoint(Entity entity) const;
		// Hinge angle (degrees) or slider offset (meters) relative to the joint's rest pose; nullopt for
		// other joint types and entities without a live joint.
		std::optional<float> GetJointPosition(Entity entity) const;

		// Closest hit along the ray, ignoring triggers and optionally one entity (e.g. the caster).
		std::optional<RaycastHit> Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, UUID ignoreEntity = 0) const;

		glm::vec3 GetGravity() const;
		void SetGravity(const glm::vec3& gravity);

		uint32_t GetBodyCount() const;
		uint64_t GetStepCount() const { return m_StepCount; }

	private:
		void RebuildDirtyJoints();
		void CreateJoint(Entity entity);
		void DispatchContacts();

	private:
		struct Impl;
		Scope<Impl> m_Impl;
		Scene* m_Scene = nullptr;
		float m_Accumulator = 0.0f;
		uint64_t m_StepCount = 0;
	};

}
