#pragma once

// Internal to the physics module.

#include "Basalt/Scene/Entity.h"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <glm/glm.hpp>

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace Basalt::PhysicsInternal {

	// Smallest collider extent, so a zero scale or size still gives Jolt a valid shape.
	constexpr float MinExtent = 0.001f;

	// An entity's colliders combined into one shape, shared by rigid bodies and characters.
	struct ColliderShape
	{
		// Null when no collider is valid (the reasons went to warn).
		JPH::Ref<JPH::Shape> Shape;
		// A MeshCollider contributed an exact triangle mesh (which has no volume).
		bool HasTriangleMesh = false;
		// The MeshComponent mesh a MeshCollider without its own Mesh used.
		std::optional<std::pair<std::string, uint32_t>> BorrowedMesh;
	};

	// Builds the entity's box, sphere, capsule and mesh colliders, scaled by its world scale (signed: mesh
	// colliders keep a mirroring) and by sizeFraction (a character's inner body is a little smaller).
	// convexReason, when set, forces mesh colliders onto their convex hull and names who needs it.
	ColliderShape BuildColliderShape(Entity entity, const glm::vec3& signedScale, float sizeFraction, const char* convexReason, bool isTrigger, const std::function<void(const std::string&)>& warn);

}
