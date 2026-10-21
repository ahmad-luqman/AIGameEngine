#include "Basalt/Renderer/RenderMath.h"

namespace Basalt {

	size_t SelectMorphWeights(std::span<const float> weights, size_t maxActive, std::vector<uint32_t>& outTargets)
	{
		outTargets.clear();
		for (uint32_t target = 0; target < weights.size(); target++)
			if (weights[target] != 0.0f && std::isfinite(weights[target]))
				outTargets.push_back(target);
		std::stable_sort(outTargets.begin(), outTargets.end(), [&](uint32_t a, uint32_t b) { return std::abs(weights[a]) > std::abs(weights[b]); });
		const size_t dropped = outTargets.size() > maxActive ? outTargets.size() - maxActive : 0;
		outTargets.resize(outTargets.size() - dropped);
		return dropped;
	}

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
