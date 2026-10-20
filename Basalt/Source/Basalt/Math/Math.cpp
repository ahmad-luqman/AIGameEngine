#include "Basalt/Math/Math.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace Basalt::Math {

	namespace {
		glm::quat RotationFromAxes(glm::vec3 (&axes)[3], glm::vec3& scale)
		{
			// A mirrored basis is represented as a negative X scale.
			if (glm::dot(glm::cross(axes[0], axes[1]), axes[2]) < 0)
			{
				scale.x = -scale.x;
				axes[0] = -axes[0];
			}
			return glm::normalize(glm::quat_cast(glm::mat3(axes[0], axes[1], axes[2])));
		}
	}

	bool NormalizeQuaternion(glm::vec4& rotation, float minimumLength)
	{
		const float length = glm::length(rotation);
		if (!std::isfinite(length) || length < minimumLength)
			return false;
		rotation /= length;
		return true;
	}

	bool IsFinite(const glm::vec2& vector)
	{
		return std::isfinite(vector.x) && std::isfinite(vector.y);
	}

	bool IsFinite(const glm::vec3& vector)
	{
		return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
	}

	bool IsFinite(const glm::quat& rotation)
	{
		return std::isfinite(rotation.x) && std::isfinite(rotation.y) && std::isfinite(rotation.z) && std::isfinite(rotation.w);
	}

	bool IsFinite(const glm::mat4& matrix)
	{
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				if (!std::isfinite(matrix[c][r]))
					return false;
		return true;
	}

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
		outRotation = RotationFromAxes(axes, scale);
		outScale = scale;
		return true;
	}

	// A zero scale leaves some rotation axes unconstrained. Complete those axes with an orthonormal
	// basis so static hierarchy entities reconstruct the authored matrix too, not only Animator poses.
	bool DecomposeAffineTransform(const glm::mat4& matrix, glm::vec3& outTranslation, glm::quat& outRotation, glm::vec3& outScale)
	{
		if (matrix[0].w != 0 || matrix[1].w != 0 || matrix[2].w != 0 || matrix[3].w != 1)
			return false;
		glm::vec3 axes[3];
		int nonzero = 0, present = 0, missing = 0;
		for (int i = 0; i < 3; i++)
		{
			axes[i] = glm::vec3(matrix[i]);
			const float length = std::hypot(axes[i].x, axes[i].y, axes[i].z);
			if (!std::isfinite(length))
				return false;
			outScale[i] = length;
			if (length > 0)
			{
				axes[i] /= length;
				present = i;
				nonzero++;
			}
			else
				missing = i;
		}
		if (nonzero == 0)
		{
			axes[0] = { 1, 0, 0 };
			axes[1] = { 0, 1, 0 };
			axes[2] = { 0, 0, 1 };
		}
		else if (nonzero == 1)
		{
			const glm::vec3 helper = std::abs(axes[present].x) < .9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
			axes[(present + 1) % 3] = glm::normalize(glm::cross(axes[present], helper));
			axes[(present + 2) % 3] = glm::cross(axes[present], axes[(present + 1) % 3]);
		}
		else if (nonzero == 2)
			axes[missing] = glm::cross(axes[(missing + 1) % 3], axes[(missing + 2) % 3]);
		// glTF node matrices must be decomposable into TRS; shear is not a node transform.
		for (int i = 0; i < 3; i++)
			if (std::abs(glm::dot(axes[i], axes[(i + 1) % 3])) > 1e-4f || std::abs(glm::length(axes[i]) - 1) > 1e-4f)
				return false;
		outTranslation = glm::vec3(matrix[3]);
		outRotation = RotationFromAxes(axes, outScale);
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
