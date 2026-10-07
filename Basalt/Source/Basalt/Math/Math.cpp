#include "Basalt/Math/Math.h"

#include <glm/gtc/matrix_transform.hpp>

namespace Basalt::Math {

	bool DecomposeTransform(const glm::mat4& transform, glm::vec3& outTranslation, glm::quat& outRotation, glm::vec3& outScale)
	{
		// Always produce usable outputs, even for degenerate input, so callers never read garbage.
		outTranslation = glm::vec3(transform[3]);
		outRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		outScale = glm::vec3(1.0f);

		if (glm::abs(transform[0][3]) > 1e-6f || glm::abs(transform[1][3]) > 1e-6f || glm::abs(transform[2][3]) > 1e-6f || glm::abs(transform[3][3] - 1.0f) > 1e-6f)
			return false; // projective matrix

		glm::vec3 axes[3] = { glm::vec3(transform[0]), glm::vec3(transform[1]), glm::vec3(transform[2]) };
		glm::vec3 scale(glm::length(axes[0]), glm::length(axes[1]), glm::length(axes[2]));
		// Absolute threshold: tiny but valid scales (deep hierarchies of 0.01-scaled nodes) still decompose.
		constexpr float MinScale = 1e-20f;
		if (scale.x < MinScale || scale.y < MinScale || scale.z < MinScale)
		{
			outScale = scale;
			return false;
		}

		for (int i = 0; i < 3; i++)
			axes[i] /= scale[i];
		// A mirrored basis is represented as a negative X scale.
		if (glm::dot(glm::cross(axes[0], axes[1]), axes[2]) < 0.0f)
		{
			scale.x = -scale.x;
			axes[0] = -axes[0];
		}

		outScale = scale;
		outRotation = glm::normalize(glm::quat_cast(glm::mat3(axes[0], axes[1], axes[2])));
		return true;
	}

	glm::mat4 ComposeTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale)
	{
		return glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
	}

	glm::quat QuatFromEuler(const glm::vec3& euler)
	{
		return glm::normalize(glm::quat(euler));
	}

	glm::vec3 EulerFromQuat(const glm::quat& rotation)
	{
		return glm::eulerAngles(rotation);
	}

}
