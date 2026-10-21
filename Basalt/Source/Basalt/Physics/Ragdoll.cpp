#include "Basalt/Physics/Ragdoll.h"

#include <cmath>
#include <unordered_set>

namespace Basalt {

	std::string ValidateRagdoll(const RagdollComponent& c)
	{
		auto range = [](float value, float min, float max) { return std::isfinite(value) && value >= min && value <= max; };
		if (!range(c.Mass, 0.01f, 100000.0f))
			return "Mass must be finite and in 0.01..100000 kg";
		if (!range(c.RadiusRatio, 0.01f, 0.5f))
			return "RadiusRatio must be finite and in 0.01..0.5";
		if (!range(c.RecoveryTime, 0, 60))
			return "RecoveryTime must be finite and in 0..60 seconds";
		if (c.Layer.empty())
			return "Layer must not be empty";
		if (c.Bones.size() > RagdollComponent::MaxBones)
			return "Bones exceeds MaxBones (128)";
		std::unordered_set<std::string> joints;
		for (size_t i = 0; i < c.Bones.size(); i++)
		{
			const auto& bone = c.Bones[i];
			const std::string prefix = "Bones[" + std::to_string(i) + "].";
			if (bone.Joint.empty() || !joints.insert(bone.Joint).second)
				return prefix + "Joint must be nonempty and unique";
			if (!range(bone.Radius, 0, 100) || !range(bone.Length, 0, 100))
				return prefix + "Radius and Length must be finite and in 0..100 model units";
			if (bone.Length > 0 && bone.Radius * 2 > bone.Length)
				return prefix + "Length must be at least twice Radius";
			if (!range(bone.Mass, 0, 100000))
				return prefix + "Mass must be finite and in 0..100000 kg";
			if (!range(bone.SwingAngle, 0, 180))
				return prefix + "SwingAngle must be finite and in 0..180 degrees";
			if (!range(bone.TwistMin, -180, 0) || !range(bone.TwistMax, 0, 180))
				return prefix + "TwistMin must be in -180..0 and TwistMax in 0..180 degrees";
		}
		return {};
	}

}
