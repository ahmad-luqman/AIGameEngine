#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Physics/PhysicsLayers.h"
#include "Basalt/Physics/PhysicsWorld.h"

#include <glm/glm.hpp>

#include <functional>

namespace Basalt {

	class Scene;

	// The static physics bodies of a scene that has no PhysicsWorld, as sphere queries see them: edit-mode previews get what
	// a playing chain would collide with without creating a world, which would write runtime records into the edit registry
	// and react to its components. It only reads the scene. Shapes are built from the same components as in Play
	// (PhysicsInternal::BuildColliderShape) and kept until those components or the entity's scale change, so moving a body
	// costs nothing. Triggers, dynamic and kinematic bodies and character controllers are left out: only static bodies stay
	// where the edit scene shows them once the game runs. Main thread only.
	class StaticPhysicsShapes
	{
	public:
		explicit StaticPhysicsShapes(Scene& scene);
		~StaticPhysicsShapes();
		StaticPhysicsShapes(const StaticPhysicsShapes&) = delete;
		StaticPhysicsShapes& operator=(const StaticPhysicsShapes&) = delete;

		// Reads the scene again: its layers, every static body's transform, and shapes whose colliders changed.
		void Refresh();
		// As PhysicsWorld::GetLayers, from the last Refresh.
		const PhysicsLayers& GetLayers() const;
		// As PhysicsWorld::CollectShapes, over the bodies as of the last Refresh.
		PhysicsShapeSnapshot CollectShapes(const glm::vec3& min, const glm::vec3& max, float radius, const PhysicsQueryFilter& filter = {}, const std::function<bool(UUID)>& accept = {}) const;

	private:
		struct Impl;
		Scope<Impl> m_Impl;
	};

}
