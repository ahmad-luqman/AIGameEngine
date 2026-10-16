#pragma once

#include "Basalt/Math/AABB.h"

#include <glm/glm.hpp>

#include <mutex>
#include <utility>
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
		// Appends line vertices (pairs) taken with TakeLines, e.g. to draw the same lines in a second view. Keeps
		// whole lines only and stops at MaxLines.
		static void AddLines(const std::vector<DebugLineVertex>& vertices);
		// Replaces the pending lines in one step (another thread's lines cannot slip in between).
		static void RestoreLines(std::vector<DebugLineVertex>&& vertices);
		static size_t GetLineCount();
		static void Clear();

		// Lines are dropped beyond this many per frame (protects against runaway scripts).
		static constexpr size_t MaxLines = 1 << 20;
	};

	// Takes the pending lines for its scope and puts them back when it ends, so a capture or a second view can
	// render without taking the lines the viewport draws this frame. Lines added inside the scope are discarded
	// when it ends, whether or not a render consumed them.
	class DebugLineStash
	{
	public:
		DebugLineStash()
			: m_Lines(DebugDraw::TakeLines())
		{
		}
		~DebugLineStash() { DebugDraw::RestoreLines(std::move(m_Lines)); }
		DebugLineStash(const DebugLineStash&) = delete;
		DebugLineStash& operator=(const DebugLineStash&) = delete;

	private:
		std::vector<DebugLineVertex> m_Lines;
	};

}
