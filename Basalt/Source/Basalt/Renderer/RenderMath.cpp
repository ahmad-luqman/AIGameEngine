#include "Basalt/Renderer/RenderMath.h"

namespace Basalt {

	Frustum::Frustum(const glm::mat4& m)
	{
		const glm::mat4 t = glm::transpose(m);
		Planes[0] = t[3] + t[0];
		Planes[1] = t[3] - t[0];
		Planes[2] = t[3] + t[1];
		Planes[3] = t[3] - t[1];
		Planes[4] = t[2]; // depth range 0..1
		Planes[5] = t[3] - t[2];
		for (glm::vec4& plane : Planes)
			plane /= glm::length(glm::vec3(plane));
	}

	bool Frustum::Intersects(const AABB& box) const
	{
		if (!box.IsValid())
			return false;
		for (const glm::vec4& plane : Planes)
		{
			const glm::vec3 positive(plane.x >= 0.0f ? box.Max.x : box.Min.x, plane.y >= 0.0f ? box.Max.y : box.Min.y, plane.z >= 0.0f ? box.Max.z : box.Min.z);
			if (glm::dot(glm::vec3(plane), positive) + plane.w < 0.0f)
				return false;
		}
		return true;
	}

}
