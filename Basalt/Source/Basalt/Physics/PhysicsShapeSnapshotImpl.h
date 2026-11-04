#pragma once

// Internal to the physics module: what PhysicsShapeSnapshot holds, filled by PhysicsWorld and StaticPhysicsShapes.

#include "Basalt/Physics/JoltUtils.h"
#include "Basalt/Physics/PhysicsWorld.h"

#include <Jolt/Jolt.h>

#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/TransformedShape.h>

#include <cmath>
#include <vector>

namespace Basalt {

	struct PhysicsShapeSnapshot::Impl
	{
		std::vector<JPH::TransformedShape> Shapes;
		JPH::Ref<JPH::Shape> Sphere;
		float Radius = 0;
		bool Truncated = false;
	};

	struct PhysicsShapeSnapshot::Candidate
	{
		// `shape` as seen from a gathered region's `center`.
		static Candidate At(UUID entity, const JPH::TransformedShape& shape, const glm::vec3& center)
		{
			const JPH::AABox bounds = shape.GetWorldSpaceBounds();
			const glm::vec3 closest = glm::clamp(center, PhysicsInternal::FromJolt(bounds.mMin), PhysicsInternal::FromJolt(bounds.mMax));
			return { .Distance = glm::length(closest - center), .Entity = entity, .Shape = shape };
		}

		// From the center of the gathered region to the shape's bounds.
		float Distance = 0;
		UUID Entity = 0;
		JPH::TransformedShape Shape;
	};

	// Whether CollectShapes can gather from a region: finite, ordered bounds and a finite, nonnegative radius.
	inline bool IsValidShapeRegion(const glm::vec3& min, const glm::vec3& max, float radius)
	{
		return PhysicsInternal::IsFiniteVec(min) && PhysicsInternal::IsFiniteVec(max) && !glm::any(glm::greaterThan(min, max)) && std::isfinite(radius) && radius >= 0.0f;
	}

}
