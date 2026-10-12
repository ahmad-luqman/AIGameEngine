#pragma once

#include "Basalt/Core/UUID.h"

#include <glm/glm.hpp>

#include <vector>

namespace Basalt {

	class Scene;

	// A scene's directional light in world space, as the renderer consumes it.
	struct DirectionalLightInfo
	{
		UUID ID = 0;
		// Direction the light travels (unit length).
		glm::vec3 Direction = { 0.0f, -1.0f, 0.0f };
		// Color * intensity.
		glm::vec3 Radiance = { 0.0f, 0.0f, 0.0f };
		bool CastShadows = false;
		float ShadowSoftness = 0.0f;
	};

	// Every directional light of the scene, primary first. Only the primary light gets shadow cascades
	// (and positions the procedural sky's sun), so it is chosen deliberately rather than by ECS storage
	// order: a shadow-casting light beats one that casts none, then the brighter light wins, then the
	// lower UUID, so the choice survives save/load and Scene::Copy. Lights whose transform collapses their
	// direction (zero scale) are skipped. The remaining lights keep that same order.
	std::vector<DirectionalLightInfo> CollectDirectionalLights(Scene& scene);

}
