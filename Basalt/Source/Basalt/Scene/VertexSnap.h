#pragma once

#include "Basalt/Core/UUID.h"

#include <glm/glm.hpp>

#include <optional>

namespace Basalt {

	class Entity;
	class Scene;

	constexpr float VertexSnapRadius = 16.0f;

	// A posed vertex in world space and top-left viewport pixels; depth is normalized to 0..1.
	struct ViewportVertex
	{
		UUID EntityId = 0;
		glm::vec3 World = glm::vec3(0);
		glm::vec2 Screen = glm::vec2(0);
		float Distance = 0;
		float Depth = 0;
	};

	// Finds the nearest projected mesh vertex within radius pixels, using the current animation pose.
	// Rejects points outside clip space. Equal screen distances prefer the vertex nearer the camera.
	std::optional<ViewportVertex> FindViewportVertex(Entity entity, const glm::mat4& viewProjection, const glm::vec2& size, const glm::vec2& pixel, float radius = VertexSnapRadius);
	// Excludes the moved entity and descendants: a target must stay stationary when the source moves.
	std::optional<ViewportVertex> FindVertexSnapTarget(Scene& scene, Entity moved, const glm::mat4& viewProjection, const glm::vec2& size, const glm::vec2& pixel, float radius = VertexSnapRadius);
	// Computes the new local translation without decomposing (and potentially shearing) rotation/scale.
	std::optional<glm::vec3> VertexSnapTranslation(Scene& scene, Entity entity, const glm::vec3& source, const glm::vec3& target);

}
