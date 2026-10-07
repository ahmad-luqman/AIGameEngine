#pragma once

#include "Basalt/Math/AABB.h"

#include <glm/glm.hpp>

#include <mutex>
#include <vector>

namespace Basalt {

	struct DebugLineVertex
	{
		glm::vec3 Position;
		glm::vec4 Color;
	};

	// Immediate-mode debug lines for the current frame (scripts, editor gizmos, tools). Lines are drawn
	// over the scene with depth testing by DebugLineRenderer and cleared after each frame.
	class DebugDraw
	{
	public:
		static void Line(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color);
		static void Box(const AABB& box, const glm::vec4& color);
		static void Box(const glm::mat4& transform, const glm::vec3& halfExtents, const glm::vec4& color);
		static void Sphere(const glm::vec3& center, float radius, const glm::vec4& color, uint32_t segments = 24);
		static void Arrow(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color);

		// Moves the accumulated lines out (renderer), leaving the list empty.
		static std::vector<DebugLineVertex> TakeLines();
		static size_t GetLineCount();
		static void Clear();

		// Lines are dropped beyond this many per frame (protects against runaway scripts).
		static constexpr size_t MaxLines = 1 << 20;
	};

}
