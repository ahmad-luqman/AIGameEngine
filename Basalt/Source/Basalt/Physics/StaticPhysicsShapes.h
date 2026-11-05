#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Physics/PhysicsLayers.h"
#include "Basalt/Physics/PhysicsWorld.h"

#include <glm/glm.hpp>

#include <functional>
#include <vector>

namespace Basalt {

	class Scene;

	// The static physics bodies of a scene that has no PhysicsWorld, as sphere queries and rays see them: edit-mode previews
	// get what a playing chain or foot would meet without creating a world, which would write runtime records into the edit registry
	// and react to its components. It only reads the scene (a collider's mesh may load through AssetManager). Shapes are built
	// from the same components as in Play (PhysicsInternal::BuildColliderShape) and kept until those components, the entity's
	// scale or layer or a mesh asset change, so a moved body rebuilds nothing. Triggers, dynamic and kinematic bodies and character
	// controllers are left out: only static bodies stay where the edit scene shows them once the game runs. Filters' triggers
	// flag is therefore moot. Main thread only.
	class StaticPhysicsShapes
	{
	public:
		// Reads the scene at once, as Refresh does.
		explicit StaticPhysicsShapes(Scene& scene);
		~StaticPhysicsShapes();
		StaticPhysicsShapes(const StaticPhysicsShapes&) = delete;
		StaticPhysicsShapes& operator=(const StaticPhysicsShapes&) = delete;

		// Reads the scene again: its layers, every static body's transform, and shapes whose inputs changed.
		void Refresh();
		// As PhysicsWorld::GetLayers, from the last Refresh.
		const PhysicsLayers& GetLayers() const;
		// As PhysicsWorld::CollectShapes, over the bodies as of the last Refresh.
		PhysicsShapeSnapshot CollectShapes(const glm::vec3& min, const glm::vec3& max, float radius, const PhysicsQueryFilter& filter = {}, const std::function<bool(UUID)>& accept = {}) const;
		// As PhysicsWorld::RaycastAll, over the bodies as of the last Refresh.
		std::vector<RaycastHit> RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const PhysicsQueryFilter& filter = {}) const;

	private:
		struct Impl;
		Scope<Impl> m_Impl;
	};

}
