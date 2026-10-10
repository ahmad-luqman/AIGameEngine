#pragma once

// Internal to the physics module.

#include "Basalt/Scene/Entity.h"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace Basalt::PhysicsInternal {

	// Smallest collider extent, so a zero scale or size still gives Jolt a valid shape.
	constexpr float MinExtent = 0.001f;

	// One per collider kind an entity can have (box, sphere, capsule, mesh).
	constexpr uint32_t MaxColliders = 4;

	// A collider's own friction and restitution (its OverrideMaterial), already clamped.
	struct ColliderMaterial
	{
		bool Override = false;
		float Friction = 0.0f;
		float Restitution = 0.0f;
	};

	// Indexed like the shape's compound children, in the order BuildColliderShape adds them; a shape made of
	// one collider is not a compound and uses index 0.
	using ColliderMaterials = std::array<ColliderMaterial, MaxColliders>;

	// An entity's colliders combined into one shape, shared by rigid bodies and characters.
	struct ColliderShape
	{
		// Null when no collider is valid (the reasons went to warn).
		JPH::Ref<JPH::Shape> Shape;
		// A MeshCollider contributed an exact triangle mesh (which has no volume).
		bool HasTriangleMesh = false;
		// The MeshComponent mesh a MeshCollider without its own Mesh used.
		std::optional<std::pair<std::string, uint32_t>> BorrowedMesh;
		ColliderMaterials Materials{};
	};

	// Clamps friction to >= 0 and restitution to [0, 1] with a warning each, so the combine modes cannot invert
	// Jolt's friction clamp or add energy on every bounce. `owner` prefixes the warnings ("BoxCollider ", or ""
	// for the RigidBody). The result has Override set.
	ColliderMaterial SanitizeSurface(float friction, float restitution, const std::string& owner, const std::function<void(const std::string&)>& warn);

	// Builds the entity's box, sphere, capsule and mesh colliders, scaled by its world scale (signed: mesh
	// colliders keep a mirroring) and by sizeFraction (a character's inner body is a little smaller).
	// convexReason, when set, forces mesh colliders onto their convex hull and names who needs it. Without
	// useMaterials (characters) collider materials are not read, and an OverrideMaterial is reported as ignored.
	ColliderShape BuildColliderShape(Entity entity, const glm::vec3& signedScale, float sizeFraction, const char* convexReason, bool isTrigger, bool useMaterials, const std::function<void(const std::string&)>& warn);

}
