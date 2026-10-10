#include "Basalt/Scene/JointFields.h"

#include <algorithm>
#include <initializer_list>
#include <iterator>

namespace Basalt {

	namespace {

		// How a joint in play takes a change to the field: applied in place, or rebuilt from the current poses
		// (which also resets the rest pose its angles are measured from).
		enum class FieldUpdate
		{
			Rebuild,
			Live
		};

		struct JointField
		{
			const char* Name;
			bool (*Differs)(const JointComponent&, const JointComponent&);
			FieldUpdate Update;
			// Whether the joint reads the field (its Type uses it and, for limit fields, UseLimits is on).
			bool (*Applies)(const JointComponent&);
		};

		bool IsAnyOf(JointType type, std::initializer_list<JointType> types)
		{
			return std::ranges::find(types, type) != types.end();
		}

		bool HasRotationMotor(const JointComponent& joint)
		{
			return joint.Type == JointType::SixDOF && std::ranges::any_of(joint.AngularMotorMode, [](JointMotorMode mode) { return mode != JointMotorMode::Off; });
		}

		// One entry per JointComponent field, in declaration order; a unit test checks this against the
		// registry's field list. A new field should rebuild unless physics applies it in place, so a field it
		// forgets to apply is still not ignored in play.
#define BS_JOINT_FIELD(name, update, ...)                                                         \
	JointField                                                                                    \
	{                                                                                             \
		#name, [](const JointComponent& a, const JointComponent& b) { return a.name != b.name; }, \
			FieldUpdate::update, [](const JointComponent& joint) -> bool { return __VA_ARGS__; }  \
	}
		const JointField s_JointFields[] = {
			BS_JOINT_FIELD(Type, Rebuild, true),
			BS_JOINT_FIELD(BodyEntity, Rebuild, true),
			BS_JOINT_FIELD(ConnectedEntity, Rebuild, true),
			BS_JOINT_FIELD(Anchor, Rebuild, true),
			BS_JOINT_FIELD(ConnectedAnchor, Rebuild, joint.Type == JointType::Distance),
			BS_JOINT_FIELD(Axis, Rebuild, IsAnyOf(joint.Type, { JointType::Hinge, JointType::Slider, JointType::Cone, JointType::SixDOF })),
			BS_JOINT_FIELD(SecondaryAxis, Rebuild, joint.Type == JointType::SixDOF),
			// Toggling UseLimits rebuilds: it changes how a distance joint's rest length is chosen.
			BS_JOINT_FIELD(UseLimits, Rebuild, !IsAnyOf(joint.Type, { JointType::Fixed, JointType::Point })),
			BS_JOINT_FIELD(LimitMin, Live, (joint.UseLimits && IsAnyOf(joint.Type, { JointType::Hinge, JointType::Slider, JointType::Distance }))),
			BS_JOINT_FIELD(LimitMax, Live, (joint.UseLimits && IsAnyOf(joint.Type, { JointType::Hinge, JointType::Slider, JointType::Distance, JointType::Cone }))),
			// Without limits a distance joint keeps its starting length as both limits, so its spring still works
			// (a bungee).
			BS_JOINT_FIELD(LimitSpringFrequency, Live, joint.Type == JointType::Distance || (joint.UseLimits && IsAnyOf(joint.Type, { JointType::Hinge, JointType::Slider, JointType::SixDOF }))),
			BS_JOINT_FIELD(LimitSpringDamping, Live, joint.Type == JointType::Distance || (joint.UseLimits && IsAnyOf(joint.Type, { JointType::Hinge, JointType::Slider, JointType::SixDOF }))),
			BS_JOINT_FIELD(LinearLimitMin, Live, (joint.UseLimits && joint.Type == JointType::SixDOF)),
			BS_JOINT_FIELD(LinearLimitMax, Live, (joint.UseLimits && joint.Type == JointType::SixDOF)),
			BS_JOINT_FIELD(AngularLimitMin, Live, (joint.UseLimits && joint.Type == JointType::SixDOF)),
			BS_JOINT_FIELD(AngularLimitMax, Live, (joint.UseLimits && joint.Type == JointType::SixDOF)),
			BS_JOINT_FIELD(FreeLinearAxes, Live, joint.Type == JointType::SixDOF),
			BS_JOINT_FIELD(MotorMode, Live, IsAnyOf(joint.Type, { JointType::Hinge, JointType::Slider })),
			BS_JOINT_FIELD(MotorTarget, Live, IsAnyOf(joint.Type, { JointType::Hinge, JointType::Slider })),
			BS_JOINT_FIELD(LinearMotorMode, Live, joint.Type == JointType::SixDOF),
			BS_JOINT_FIELD(AngularMotorMode, Live, joint.Type == JointType::SixDOF),
			BS_JOINT_FIELD(LinearMotorTarget, Live, joint.Type == JointType::SixDOF),
			BS_JOINT_FIELD(AngularMotorTarget, Live, joint.Type == JointType::SixDOF),
			// A hinge's motor is capped by MotorMaxForce only while MotorMaxTorque is not positive.
			BS_JOINT_FIELD(MotorMaxForce, Live, IsAnyOf(joint.Type, { JointType::Slider, JointType::SixDOF }) || (joint.Type == JointType::Hinge && joint.MotorMaxTorque <= 0.0f)),
			BS_JOINT_FIELD(MotorMaxTorque, Live, IsAnyOf(joint.Type, { JointType::Hinge, JointType::SixDOF })),
			BS_JOINT_FIELD(MotorSpringFrequency, Live, IsAnyOf(joint.Type, { JointType::Hinge, JointType::Slider, JointType::SixDOF })),
			BS_JOINT_FIELD(MotorSpringDamping, Live, IsAnyOf(joint.Type, { JointType::Hinge, JointType::Slider, JointType::SixDOF })),
			BS_JOINT_FIELD(BreakForce, Live, true),
			// Point and distance joints hold no torque, and neither do cone and six-DOF joints that rotate freely
			// (a six-DOF rotation motor makes it hold torque).
			BS_JOINT_FIELD(BreakTorque, Live, IsAnyOf(joint.Type, { JointType::Fixed, JointType::Hinge, JointType::Slider }) || (joint.UseLimits && IsAnyOf(joint.Type, { JointType::Cone, JointType::SixDOF })) || HasRotationMotor(joint)),
			BS_JOINT_FIELD(EnableCollision, Live, true),
		};
#undef BS_JOINT_FIELD

		const JointField* FindField(std::string_view name)
		{
			auto it = std::ranges::find_if(s_JointFields, [name](const JointField& field) { return name == field.Name; });
			return it != std::end(s_JointFields) ? &*it : nullptr;
		}

	}

	bool JointFieldApplies(const JointComponent& joint, std::string_view field)
	{
		const JointField* entry = FindField(field);
		return !entry || entry->Applies(joint);
	}

	bool JointNeedsRebuild(const JointComponent& built, const JointComponent& current)
	{
		return std::ranges::any_of(s_JointFields, [&](const JointField& field) { return field.Update == FieldUpdate::Rebuild && field.Differs(built, current); });
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
			if (field.Differs(joint, defaults) && !field.Applies(joint))
				ignored.emplace_back(field.Name);
		}
		return ignored;
	}

}
