#pragma once

// Internal to the physics module.

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <cstdint>
#include <string>

namespace Basalt::PhysicsInternal {

	// Cooked mesh collider shapes, shared by every entity (and every PhysicsWorld) that uses the same mesh.
	// Cooking a level's triangle tree is the slow part of starting play, so shapes outlive the world; an
	// entry is re-cooked when its mesh asset was reloaded. Main thread only (it reads the AssetManager).

	// Returns the unscaled shape for one mesh of a mesh asset, cooking it on first use. On failure returns
	// null and sets outError.
	JPH::Ref<JPH::Shape> GetMeshShape(const std::string& key, uint32_t meshIndex, bool convex, std::string& outError);
	// How many shapes have been cooked so far (tests use it to check the cache).
	uint64_t GetMeshShapeCookCount();
	// Drops every cached shape; shapes still used by a body stay alive through that body.
	void ClearMeshShapeCache();

}
