#pragma once

// Internal to the physics module: checks and conversions of JointComponent settings for Jolt.

#include "Basalt/Core/UUID.h"
#include "Basalt/Scene/Components.h"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Constraints/MotorSettings.h>
#include <Jolt/Physics/Constraints/SpringSettings.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <string>
#include <utility>
#include <vector>

namespace Basalt::PhysicsInternal {

	// Whether a joint change needs a new constraint. Only the fields a live Jolt constraint can update
	// are exempt, so a field added later rebuilds the joint by default instead of being ignored in play.
	// (Toggling UseLimits rebuilds too: it changes how a distance joint's rest length is chosen.)
	bool NeedsRebuild(const JointComponent& built, const JointComponent& current);

	// The entity whose body a joint moves: BodyEntity, or the entity holding the component when it is 0.
	UUID JointBody(UUID holder, const JointComponent& joint);

	const char* JointTypeName(JointType type);

	// Limits Jolt accepts for the joint (it asserts on others). Hinge and slider limits must contain
	// the rest pose (0); distance limits are lengths; a cone has only its half angle. Changed values are
	// reported in warnings; fields a joint ignores are reported by ReportIgnoredFields.
	std::pair<float, float> SanitizeLimits(const JointComponent& joint, std::vector<std::string>& warnings);

	// Fields set away from their defaults that do nothing for this joint (see JointFieldApplies), split by
	// whether turning on UseLimits would make them count.
	void ReportIgnoredFields(const JointComponent& joint, std::vector<std::string>& warnings);

	// Six-DOF limits in Jolt's units (meters, radians) along/around the joint frame's axes.
	struct SixDOFLimits
	{
		glm::vec3 LinearMin = { 0.0f, 0.0f, 0.0f };
		glm::vec3 LinearMax = { 0.0f, 0.0f, 0.0f };
		glm::vec3 AngularMin = { -glm::pi<float>(), -glm::pi<float>(), -glm::pi<float>() };
		glm::vec3 AngularMax = { glm::pi<float>(), glm::pi<float>(), glm::pi<float>() };
	};

	constexpr const char* s_AxisNames[] = { "X", "Y", "Z" };

	// Without UseLimits translation is locked and rotation free (a point joint). With limits every range
	// must contain the rest pose (0), twist stays within +-180 degrees, and the swing cone is symmetric
	// (Jolt's cone swing ignores the minimum), so a swing minimum other than 0 or -max is mirrored.
	// Angles are capped at pi: Jolt asserts on more, and glm::radians(180) can round past it.
	// FreeLinearAxes free their translation axes either way (Jolt treats +-FLT_MAX as free).
	SixDOFLimits SanitizeSixDOFLimits(const JointComponent& joint, std::vector<std::string>& warnings);

	// The spring that softens a joint's limits (a frequency of 0 keeps them rigid). Whether the joint has
	// limits to soften is reported by ReportIgnoredFields.
	JPH::SpringSettings SanitizeLimitSpring(const JointComponent& joint, std::vector<std::string>& warnings);

	// The spring a Position motor pulls toward its target with. Jolt switches a position motor off when its
	// spring has no stiffness, so the frequency must be positive.
	JPH::SpringSettings SanitizeMotorSpring(const JointComponent& joint, std::vector<std::string>& warnings);

	JPH::EMotorState ToJoltMotorState(JointMotorMode mode);

}
