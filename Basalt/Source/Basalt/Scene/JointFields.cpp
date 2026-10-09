#include "Basalt/Scene/JointFields.h"

#include <algorithm>
#include <initializer_list>

namespace Basalt {

	namespace {

		struct JointField
		{
			const char* Name;
			bool (*Differs)(const JointComponent&, const JointComponent&);
		};

		// One entry per JointComponent field; a unit test checks this against the registry's field list.
#define BS_JOINT_FIELD(name)                                                                     \
	JointField                                                                                   \
	{                                                                                            \
		#name, [](const JointComponent& a, const JointComponent& b) { return a.name != b.name; } \
	}
		const JointField s_JointFields[] = {
			BS_JOINT_FIELD(Type),
			BS_JOINT_FIELD(BodyEntity),
			BS_JOINT_FIELD(ConnectedEntity),
			BS_JOINT_FIELD(Anchor),
			BS_JOINT_FIELD(ConnectedAnchor),
			BS_JOINT_FIELD(Axis),
			BS_JOINT_FIELD(SecondaryAxis),
			BS_JOINT_FIELD(UseLimits),
			BS_JOINT_FIELD(LimitMin),
			BS_JOINT_FIELD(LimitMax),
			BS_JOINT_FIELD(LimitSpringFrequency),
			BS_JOINT_FIELD(LimitSpringDamping),
			BS_JOINT_FIELD(LinearLimitMin),
			BS_JOINT_FIELD(LinearLimitMax),
			BS_JOINT_FIELD(AngularLimitMin),
			BS_JOINT_FIELD(AngularLimitMax),
			BS_JOINT_FIELD(FreeLinearAxes),
			BS_JOINT_FIELD(MotorMode),
			BS_JOINT_FIELD(MotorTarget),
			BS_JOINT_FIELD(LinearMotorMode),
			BS_JOINT_FIELD(AngularMotorMode),
			BS_JOINT_FIELD(LinearMotorTarget),
			BS_JOINT_FIELD(AngularMotorTarget),
			BS_JOINT_FIELD(MotorMaxForce),
			BS_JOINT_FIELD(MotorSpringFrequency),
			BS_JOINT_FIELD(MotorSpringDamping),
			BS_JOINT_FIELD(BreakForce),
			BS_JOINT_FIELD(BreakTorque),
			BS_JOINT_FIELD(EnableCollision),
		};
#undef BS_JOINT_FIELD

		bool IsAnyOf(JointType type, std::initializer_list<JointType> types)
		{
			return std::ranges::find(types, type) != types.end();
		}

	}

	bool JointFieldApplies(const JointComponent& joint, std::string_view field)
	{
		const JointType type = joint.Type;
		const bool limited = joint.UseLimits;
		if (field == "ConnectedAnchor")
			return type == JointType::Distance;
		if (field == "Axis")
			return IsAnyOf(type, { JointType::Hinge, JointType::Slider, JointType::Cone, JointType::SixDOF });
		if (field == "SecondaryAxis")
			return type == JointType::SixDOF;
		if (field == "UseLimits")
			return !IsAnyOf(type, { JointType::Fixed, JointType::Point });
		if (field == "LimitMin")
			return limited && IsAnyOf(type, { JointType::Hinge, JointType::Slider, JointType::Distance });
		if (field == "LimitMax")
			return limited && IsAnyOf(type, { JointType::Hinge, JointType::Slider, JointType::Distance, JointType::Cone });
		// Without limits a distance joint keeps its starting length as both limits, so its spring still works
		// (a bungee).
		if (field.starts_with("LimitSpring"))
			return type == JointType::Distance || (limited && IsAnyOf(type, { JointType::Hinge, JointType::Slider, JointType::SixDOF }));
		if (field.starts_with("LinearLimit") || field.starts_with("AngularLimit"))
			return limited && type == JointType::SixDOF;
		if (field == "FreeLinearAxes" || field.starts_with("LinearMotor") || field.starts_with("AngularMotor"))
			return type == JointType::SixDOF;
		if (field == "MotorMode" || field == "MotorTarget")
			return IsAnyOf(type, { JointType::Hinge, JointType::Slider });
		if (field.starts_with("Motor"))
			return IsAnyOf(type, { JointType::Hinge, JointType::Slider, JointType::SixDOF });
		// Point and distance joints hold no torque, and neither do cone and six-DOF joints that rotate freely
		// (a six-DOF rotation motor makes it hold torque).
		if (field == "BreakTorque")
		{
			const bool rotationMotor = type == JointType::SixDOF && std::ranges::any_of(joint.AngularMotorMode, [](JointMotorMode mode) { return mode != JointMotorMode::Off; });
			return IsAnyOf(type, { JointType::Fixed, JointType::Hinge, JointType::Slider }) || (limited && IsAnyOf(type, { JointType::Cone, JointType::SixDOF })) || rotationMotor;
		}
		return true;
	}

	const std::vector<std::string>& GetJointFieldNames()
	{
		static const std::vector<std::string> s_Names = [] {
			std::vector<std::string> names;
			for (const JointField& field : s_JointFields)
				names.emplace_back(field.Name);
			return names;
		}();
		return s_Names;
	}

	std::vector<std::string> GetIgnoredJointFields(const JointComponent& joint)
	{
		const JointComponent defaults;
		std::vector<std::string> ignored;
		for (const JointField& field : s_JointFields)
		{
			if (field.Differs(joint, defaults) && !JointFieldApplies(joint, field.Name))
				ignored.emplace_back(field.Name);
		}
		return ignored;
	}

}
