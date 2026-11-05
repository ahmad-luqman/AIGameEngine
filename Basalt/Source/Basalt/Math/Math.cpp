#include "Basalt/Math/Math.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Jolt.h must precede every other Jolt header.
#include <Jolt/Jolt.h>

#include <Jolt/Math/Quat.h>

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
			// In double, which cannot overflow and rounds alike everywhere (libm's hypot does not): these lengths reach physics.
			const double x = axes[i].x, y = axes[i].y, z = axes[i].z;
			const float length = static_cast<float>(std::sqrt(x * x + y * y + z * z));
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
		// Doubles, so the float angles do not depend on how each platform's float atan2 rounds, and the rotation matrix
		// entries near gimbal lock keep the digits that decide the result.
		const double length = std::sqrt(static_cast<double>(rotation.w) * rotation.w + static_cast<double>(rotation.x) * rotation.x + static_cast<double>(rotation.y) * rotation.y + static_cast<double>(rotation.z) * rotation.z);
		if (length == 0)
			return glm::vec3(0.0f);
		const double w = rotation.w / length, x = rotation.x / length, y = rotation.y / length, z = rotation.z / length;
		const double m00 = 1 - 2 * (y * y + z * z), m01 = 2 * (x * y - w * z), m02 = 2 * (x * z + w * y);
		const double m10 = 2 * (x * y + w * z), m11 = 1 - 2 * (x * x + z * z), m12 = 2 * (y * z - w * x);
		const double sinYaw = 2 * (w * y - x * z);
		// Taken from the first column rather than as asin(sinYaw), which loses half its digits near +-90 degrees.
		const double cosYaw = std::hypot(m00, m10);
		// Float rounding leaves up to about 5e-7 here at an exact lock, so this finds every one and nothing more than 6e-5
		// degrees from it. Pitch and roll turn about one axis there: roll is 0, by the same convention glm used.
		constexpr double LockCosine = 1e-6;
		const bool locked = cosYaw < LockCosine;
		const double roll = locked ? 0.0 : std::atan2(m10, m00);
		const double yaw = locked ? std::copysign(glm::half_pi<double>(), sinYaw) : std::atan2(sinYaw, cosYaw);
		// Pitch and roll each come from entries that shrink to rounding noise near lock, so read independently (as
		// glm::eulerAngles does) they can be noise that does not add up: 180 degrees off at worst. Pitch here is what is
		// left once this roll is undone, so whatever roll turned out to be, the two together are the rotation.
		const double sinRoll = std::sin(roll), cosRoll = std::cos(roll);
		double pitch = std::atan2(sinRoll * m02 - cosRoll * m12, cosRoll * m11 - sinRoll * m01);
		if (locked)
		{
			// A saved rotation must read back as the same text. A float quaternion holds pitch to about 2e-5 degrees, under
			// half of this grid's step, so each step rounds back to itself (a test runs this at many pitches).
			constexpr double DegreesPerRadian = 180.0 / glm::pi<double>();
			constexpr double StepsPerDegree = 1e4;
			double steps = std::round(pitch * DegreesPerRadian * StepsPerDegree);
			// +180 and -180 are one pitch, and which side of it the sign falls on is rounding noise.
			if (steps <= -180 * StepsPerDegree)
				steps = 180 * StepsPerDegree;
			pitch = steps / StepsPerDegree / DegreesPerRadian;
		}
		// Adding 0 after the cast, which can round a tiny angle to -0, or every unrotated entity would save a "-0.0".
		const auto toFloat = [](double angle) { return static_cast<float>(angle) + 0.0f; };
		return glm::vec3(toFloat(pitch), toFloat(yaw), toFloat(roll));
	}

	float Atan2(float y, float x)
	{
		// Jolt's divides by what would be 0 here, where atan2's convention is 0 too.
		return x == 0 && y == 0 ? 0.0f : JPH::ATan2(y, x);
	}

	glm::quat YawRotation(float yaw)
	{
		return glm::quat(JPH::Cos(0.5f * yaw), 0, JPH::Sin(0.5f * yaw), 0);
	}

	glm::quat Slerp(const glm::quat& from, const glm::quat& to, float weight)
	{
		const JPH::Quat result = JPH::Quat(from.x, from.y, from.z, from.w).SLERP(JPH::Quat(to.x, to.y, to.z, to.w), weight);
		return glm::quat(result.GetW(), result.GetX(), result.GetY(), result.GetZ());
	}

}
