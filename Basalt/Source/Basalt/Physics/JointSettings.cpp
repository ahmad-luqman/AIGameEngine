#include "Basalt/Physics/JointSettings.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Scene/JointFields.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace Basalt::PhysicsInternal {

	UUID JointBody(UUID holder, const JointComponent& joint)
	{
		return joint.BodyEntity != 0 ? joint.BodyEntity : holder;
	}

	const char* JointTypeName(JointType type)
	{
		switch (type)
		{
			case JointType::Fixed:
				return "fixed";
			case JointType::Point:
				return "point";
			case JointType::Hinge:
				return "hinge";
			case JointType::Slider:
				return "slider";
			case JointType::Distance:
				return "distance";
			case JointType::Cone:
				return "cone";
			case JointType::SixDOF:
				return "six-DOF";
		}
		return "unknown";
	}

	std::pair<float, float> SanitizeLimits(const JointComponent& joint, std::vector<std::string>& warnings)
	{
		if (!joint.UseLimits)
			return { 0.0f, 0.0f };
		switch (joint.Type)
		{
			case JointType::Hinge:
			case JointType::Slider:
			{
				const bool hinge = joint.Type == JointType::Hinge;
				const float range = hinge ? 180.0f : FLT_MAX;
				const float min = std::clamp(joint.LimitMin, -range, 0.0f);
				const float max = std::clamp(joint.LimitMax, 0.0f, range);
				if (min != joint.LimitMin || max != joint.LimitMax)
					warnings.emplace_back(fmt::format("{} limits [{}, {}] must satisfy {} (the rest pose is 0); clamped to [{}, {}]", JointTypeName(joint.Type), joint.LimitMin,
													  joint.LimitMax, hinge ? "-180 <= LimitMin <= 0 <= LimitMax <= 180 degrees" : "LimitMin <= 0 <= LimitMax", min, max));
				return { min, max };
			}
			case JointType::Distance:
			{
				const float min = std::max(joint.LimitMin, 0.0f);
				const float max = std::max(joint.LimitMax, min);
				if (min != joint.LimitMin || max != joint.LimitMax)
					warnings.emplace_back(fmt::format("distance limits [{}, {}] must satisfy 0 <= LimitMin <= LimitMax; clamped to [{}, {}]", joint.LimitMin, joint.LimitMax, min, max));
				if (max <= 0.0f)
					warnings.emplace_back("distance limits with LimitMax <= 0 pull the anchors together");
				return { min, max };
			}
			case JointType::Cone:
			{
				// Only the half angle; the cone is centered on the rest direction.
				const float max = std::clamp(joint.LimitMax, 0.0f, 180.0f);
				if (max != joint.LimitMax)
					warnings.emplace_back(fmt::format("cone half angle LimitMax {} must be within [0, 180] degrees; clamped to {}", joint.LimitMax, max));
				return { 0.0f, max };
			}
			case JointType::SixDOF:
			case JointType::Fixed:
			case JointType::Point:
				break;
		}
		return { 0.0f, 0.0f };
	}

	void ReportIgnoredFields(const JointComponent& joint, std::vector<std::string>& warnings)
	{
		JointComponent limited = joint;
		limited.UseLimits = true;
		std::string byType;
		std::string byLimits;
		int byTypeCount = 0;
		int byLimitsCount = 0;
		for (const std::string& field : GetIgnoredJointFields(joint))
		{
			const bool needsLimits = JointFieldApplies(limited, field);
			std::string& list = needsLimits ? byLimits : byType;
			list += (list.empty() ? "" : ", ") + field;
			(needsLimits ? byLimitsCount : byTypeCount)++;
		}
		if (byTypeCount > 0)
			warnings.emplace_back(fmt::format("{} {} no effect on {} joints", byType, byTypeCount == 1 ? "has" : "have", JointTypeName(joint.Type)));
		if (byLimitsCount > 0)
			warnings.emplace_back(fmt::format("{} {} no effect without UseLimits", byLimits, byLimitsCount == 1 ? "has" : "have"));
	}

	SixDOFLimits SanitizeSixDOFLimits(const JointComponent& joint, std::vector<std::string>& warnings)
	{
		SixDOFLimits limits;
		for (int i = 0; i < 3; i++)
		{
			if (!joint.FreeLinearAxes[i])
				continue;
			limits.LinearMin[i] = -FLT_MAX;
			limits.LinearMax[i] = FLT_MAX;
			if (joint.UseLimits && (joint.LinearLimitMin[i] != 0.0f || joint.LinearLimitMax[i] != 0.0f))
				warnings.emplace_back(fmt::format("six-DOF linear {} limits are ignored: the axis is free (FreeLinearAxes)", s_AxisNames[i]));
		}
		if (!joint.UseLimits)
			return limits;
		for (int i = 0; i < 3; i++)
		{
			if (!joint.FreeLinearAxes[i])
			{
				const float linearMin = std::min(joint.LinearLimitMin[i], 0.0f);
				const float linearMax = std::max(joint.LinearLimitMax[i], 0.0f);
				if (linearMin != joint.LinearLimitMin[i] || linearMax != joint.LinearLimitMax[i])
					warnings.emplace_back(fmt::format("six-DOF linear {} limits [{}, {}] must satisfy min <= 0 <= max (the rest pose is 0); clamped to [{}, {}]", s_AxisNames[i],
													  joint.LinearLimitMin[i], joint.LinearLimitMax[i], linearMin, linearMax));
				limits.LinearMin[i] = linearMin;
				limits.LinearMax[i] = linearMax;
			}

			float angularMin = 0.0f;
			const float angularMax = std::clamp(joint.AngularLimitMax[i], 0.0f, 180.0f);
			if (i == 0)
			{
				angularMin = std::clamp(joint.AngularLimitMin[i], -180.0f, 0.0f);
				if (angularMin != joint.AngularLimitMin[i] || angularMax != joint.AngularLimitMax[i])
					warnings.emplace_back(fmt::format("six-DOF twist (angular X) limits [{}, {}] must satisfy -180 <= min <= 0 <= max <= 180 degrees; clamped to [{}, {}]", joint.AngularLimitMin[i],
													  joint.AngularLimitMax[i], angularMin, angularMax));
			}
			else
			{
				// A minimum of 0 (the default) stands for -max, so setting only the maximum is enough.
				angularMin = -angularMax;
				const bool mirrored = joint.AngularLimitMin[i] == 0.0f || joint.AngularLimitMin[i] == angularMin;
				if (!mirrored || angularMax != joint.AngularLimitMax[i])
					warnings.emplace_back(fmt::format("six-DOF swing (angular {}) limits [{}, {}] must be a symmetric half angle [-max, max] with 0 <= max <= 180 degrees; using [{}, {}]", s_AxisNames[i],
													  joint.AngularLimitMin[i], joint.AngularLimitMax[i], angularMin, angularMax));
			}
			limits.AngularMin[i] = std::max(glm::radians(angularMin), -JPH::JPH_PI);
			limits.AngularMax[i] = std::min(glm::radians(angularMax), JPH::JPH_PI);
		}
		return limits;
	}

	JPH::SpringSettings SanitizeLimitSpring(const JointComponent& joint, std::vector<std::string>& warnings)
	{
		if (joint.LimitSpringFrequency < 0.0f)
			warnings.emplace_back(fmt::format("LimitSpringFrequency {} is negative; using 0 (rigid limits)", joint.LimitSpringFrequency));
		if (joint.LimitSpringDamping < 0.0f)
			warnings.emplace_back(fmt::format("LimitSpringDamping {} is negative; using 0", joint.LimitSpringDamping));
		const float frequency = std::max(joint.LimitSpringFrequency, 0.0f);
		const float damping = std::max(joint.LimitSpringDamping, 0.0f);
		return JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, frequency, damping);
	}

	JPH::SpringSettings SanitizeMotorSpring(const JointComponent& joint, std::vector<std::string>& warnings)
	{
		const float defaultFrequency = JointComponent().MotorSpringFrequency;
		const bool validFrequency = joint.MotorSpringFrequency > 0.0f;
		if (!validFrequency)
			warnings.emplace_back(fmt::format("MotorSpringFrequency {} must be greater than 0; using {}", joint.MotorSpringFrequency, defaultFrequency));
		if (joint.MotorSpringDamping < 0.0f)
			warnings.emplace_back(fmt::format("MotorSpringDamping {} is negative; using 0", joint.MotorSpringDamping));
		return JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, validFrequency ? joint.MotorSpringFrequency : defaultFrequency, std::max(joint.MotorSpringDamping, 0.0f));
	}

	JPH::EMotorState ToJoltMotorState(JointMotorMode mode)
	{
		switch (mode)
		{
			case JointMotorMode::Velocity:
				return JPH::EMotorState::Velocity;
			case JointMotorMode::Position:
				return JPH::EMotorState::Position;
			case JointMotorMode::Off:
				break;
		}
		return JPH::EMotorState::Off;
	}

}
