#pragma once

// Internal to the physics module: what PhysicsShapeSnapshot holds, filled by PhysicsWorld and StaticPhysicsShapes.

#include "Basalt/Physics/PhysicsWorld.h"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/TransformedShape.h>

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
		// From the center of the gathered region to the shape's bounds.
		float Distance = 0;
		UUID Entity = 0;
		JPH::TransformedShape Shape;
	};

}
