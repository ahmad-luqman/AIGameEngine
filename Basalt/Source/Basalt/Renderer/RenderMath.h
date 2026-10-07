#pragma once

#include "Basalt/Math/AABB.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace Basalt {

	// View-frustum planes (ax + by + cz + d >= 0 is inside) of a view-projection matrix with a 0..1
	// depth range.
	struct Frustum
	{
		glm::vec4 Planes[6];

		explicit Frustum(const glm::mat4& viewProjection);
		bool Intersects(const AABB& box) const;
	};

	// True for orthographic projections (no perspective divide).
	inline bool IsOrthographic(const glm::mat4& projection)
	{
		return projection[3][3] == 1.0f;
	}

	// View-space distances at which shadow cascades end, covering [near, shadowFar]. Perspective cameras use
	// the practical split scheme (lambda blends logarithmic and uniform splits); orthographic cameras and
	// cameras with a non-positive near plane use uniform splits (the logarithm is undefined there).
	template<size_t Count>
	std::array<float, Count> ComputeCascadeSplits(float nearClip, float shadowFar, float lambda, bool orthographic)
	{
		std::array<float, Count> splits{};
		const bool logarithmic = !orthographic && nearClip > 0.0f && shadowFar > nearClip;
		const float farClip = std::max(shadowFar, nearClip + 1e-3f);
		for (size_t i = 0; i < Count; i++)
		{
			const float p = static_cast<float>(i + 1) / static_cast<float>(Count);
			const float uniform = nearClip + (farClip - nearClip) * p;
			splits[i] = logarithmic ? lambda * (nearClip * std::pow(farClip / nearClip, p)) + (1.0f - lambda) * uniform : uniform;
		}
		return splits;
	}

}
