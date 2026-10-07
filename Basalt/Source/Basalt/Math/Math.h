#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Basalt::Math {

	// Splits an affine transform into translation, rotation and scale (a mirrored basis becomes a negative
	// X scale). Returns false for degenerate matrices (zero scale or projective components); the outputs
	// are then still well-defined (translation preserved, identity rotation).
	bool DecomposeTransform(const glm::mat4& transform, glm::vec3& outTranslation, glm::quat& outRotation, glm::vec3& outScale);

	glm::mat4 ComposeTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale);

	// Euler angles in radians (pitch = X, yaw = Y, roll = Z), applied in the order used by glm::quat(eulerAngles).
	glm::quat QuatFromEuler(const glm::vec3& euler);
	glm::vec3 EulerFromQuat(const glm::quat& rotation);

	inline bool NearlyEqual(float a, float b, float epsilon = 1e-5f)
	{
		return glm::abs(a - b) <= epsilon;
	}

}
