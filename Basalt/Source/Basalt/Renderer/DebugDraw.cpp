#include "Basalt/Renderer/DebugDraw.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace Basalt {

	namespace {

		std::mutex s_Mutex;
		std::vector<DebugLineVertex> s_Vertices;

	}

	void DebugDraw::Line(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color)
	{
		std::scoped_lock lock(s_Mutex);
		if (s_Vertices.size() / 2 >= MaxLines)
			return;
		s_Vertices.push_back({ from, color });
		s_Vertices.push_back({ to, color });
	}

	void DebugDraw::Box(const AABB& box, const glm::vec4& color)
	{
		if (!box.IsValid())
			return;
		const glm::vec3 center = box.GetCenter();
		const glm::vec3 extents = box.GetExtents();
		Box(glm::translate(glm::mat4(1.0f), center), extents, color);
	}

	void DebugDraw::Box(const glm::mat4& transform, const glm::vec3& halfExtents, const glm::vec4& color)
	{
		glm::vec3 corners[8];
		for (int i = 0; i < 8; i++)
		{
			const glm::vec3 local((i & 1) ? halfExtents.x : -halfExtents.x, (i & 2) ? halfExtents.y : -halfExtents.y, (i & 4) ? halfExtents.z : -halfExtents.z);
			corners[i] = glm::vec3(transform * glm::vec4(local, 1.0f));
		}
		const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
		for (const auto& edge : edges)
			Line(corners[edge[0]], corners[edge[1]], color);
	}

	void DebugDraw::Sphere(const glm::vec3& center, float radius, const glm::vec4& color, uint32_t segments)
	{
		segments = std::max(segments, 4u);
		for (int axis = 0; axis < 3; axis++)
		{
			glm::vec3 previous;
			for (uint32_t i = 0; i <= segments; i++)
			{
				const float angle = static_cast<float>(i) / static_cast<float>(segments) * glm::two_pi<float>();
				const float c = glm::cos(angle) * radius;
				const float s = glm::sin(angle) * radius;
				const glm::vec3 point = center + (axis == 0 ? glm::vec3(0.0f, c, s) : (axis == 1 ? glm::vec3(c, 0.0f, s) : glm::vec3(c, s, 0.0f)));
				if (i > 0)
					Line(previous, point, color);
				previous = point;
			}
		}
	}

	void DebugDraw::Arrow(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color)
	{
		Line(from, to, color);
		const glm::vec3 direction = to - from;
		const float length = glm::length(direction);
		if (length < 1e-6f)
			return;
		const glm::vec3 forward = direction / length;
		const glm::vec3 side = glm::normalize(glm::abs(forward.y) < 0.99f ? glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)) : glm::cross(forward, glm::vec3(1.0f, 0.0f, 0.0f)));
		const glm::vec3 up = glm::cross(side, forward);
		const float head = length * 0.15f;
		for (const glm::vec3& offset : { side, -side, up, -up })
			Line(to, to - forward * head + offset * head * 0.5f, color);
	}

	std::vector<DebugLineVertex> DebugDraw::TakeLines()
	{
		std::scoped_lock lock(s_Mutex);
		std::vector<DebugLineVertex> result;
		result.swap(s_Vertices);
		return result;
	}

	size_t DebugDraw::GetLineCount()
	{
		std::scoped_lock lock(s_Mutex);
		return s_Vertices.size() / 2;
	}

	void DebugDraw::Clear()
	{
		std::scoped_lock lock(s_Mutex);
		s_Vertices.clear();
	}

}
