#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Breccia::Math {

	// Splits an affine transform into translation, rotation and scale (a mirrored basis becomes a negative
	// X scale). Returns false for degenerate matrices (zero scale or projective components); the outputs
	// are then still well-defined (translation preserved, identity rotation).
	bool DecomposeTransform(const glm::mat4& transform, glm::vec3& outTranslation, glm::quat& outRotation, glm::vec3& outScale);

	// Strict TRS decomposition for imported affine matrices, including zero-scale axes; rejects shear.
	bool DecomposeAffineTransform(const glm::mat4& transform, glm::vec3& outTranslation, glm::quat& outRotation, glm::vec3& outScale);
	bool IsFinite(const glm::mat4& matrix);
	bool IsFinite(const glm::vec2& vector);
	bool IsFinite(const glm::vec3& vector);
	bool IsFinite(const glm::quat& rotation);

	// Normalizes an xyzw rotation, leaving it unchanged when its length is non-finite or too small.
	bool NormalizeQuaternion(glm::vec4& rotation, float minimumLength = 1e-6f);

	glm::mat4 ComposeTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale);

	// Euler angles in radians (pitch = X, yaw = Y, roll = Z), applied in the order used by glm::quat(eulerAngles).
	glm::quat QuatFromEuler(const glm::vec3& euler);
	// Any rotation (normalized here; a zero quaternion is no rotation) as angles QuatFromEuler turns back into it. At gimbal
	// lock (yaw +-90 degrees) roll is 0, yaw is exact and pitch sits on a 1e-4 degree grid, with 180 rather than -180, so what
	// this returns there comes back as the same bits; elsewhere the last bits may differ, never the orientation by more than
	// rounding.
	glm::vec3 EulerFromQuat(const glm::quat& rotation);

	// Shortest-arc spherical interpolation (weight 0: from, 1: to), normalized, computed with Jolt's portable trigonometry so
	// it rounds the same on every platform (libm's acos and sin do not): sampled poses can drive deterministic physics.
	glm::quat Slerp(const glm::quat& from, const glm::quat& to, float weight);
	// atan2 and a turn about +Y that round the same on every platform (Jolt's own trigonometry, not libm): root motion reaches
	// physics through characters and kinematic ragdoll roots.
	float Atan2(float y, float x);
	glm::quat YawRotation(float yaw);

	inline bool NearlyEqual(float a, float b, float epsilon = 1e-5f)
	{
		return glm::abs(a - b) <= epsilon;
	}

}
