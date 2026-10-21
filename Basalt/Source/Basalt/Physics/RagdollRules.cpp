#include "Basalt/Physics/RagdollRules.h"

#include "Basalt/Math/Math.h"
#include "Basalt/Physics/JoltUtils.h"

#include <spdlog/fmt/fmt.h>

#include <cmath>
#include <unordered_set>

namespace Basalt {

	bool DecomposeRagdollTransform(const glm::mat4& matrix, glm::vec3& position, glm::quat& rotation, glm::vec3& scale)
	{
		if (!Math::IsFinite(matrix) || matrix[0].w != 0 || matrix[1].w != 0 || matrix[2].w != 0 || matrix[3].w != 1 ||
			!Math::DecomposeTransform(matrix, position, rotation, scale) || !Math::IsFinite(scale) || !Math::IsFinite(rotation))
			return false;
		for (int i = 0; i < 3; i++)
		{
			const glm::vec3 a = glm::vec3(matrix[i]) / std::abs(scale[i]);
			const glm::vec3 b = glm::vec3(matrix[(i + 1) % 3]) / std::abs(scale[(i + 1) % 3]);
			if (std::abs(glm::dot(a, b)) > 1e-4f)
				return false;
		}
		return true;
	}

	glm::quat BlendRagdollRotation(const glm::quat& animated, const glm::quat& physical, float weight)
	{
		return PhysicsInternal::FromJolt(PhysicsInternal::ToJolt(animated).SLERP(PhysicsInternal::ToJolt(physical), weight));
	}

	std::string ValidateRagdoll(const RagdollComponent& c)
	{
		auto range = [](float value, float min, float max) { return std::isfinite(value) && value >= min && value <= max; };
		if (!range(c.Mass, 0.01f, 100000.0f))
			return "Mass must be finite and in 0.01..100000 kg";
		if (!range(c.RadiusRatio, 0.01f, 0.5f))
			return "RadiusRatio must be finite and in 0.01..0.5";
		if (!range(c.RecoveryTime, 0, RagdollComponent::MaxRecoveryTime))
			return fmt::format("RecoveryTime must be finite and in 0..{} seconds", RagdollComponent::MaxRecoveryTime);
		if (c.Layer.empty())
			return "Layer must not be empty";
		if (c.Bones.size() > RagdollComponent::MaxBones)
			return "Bones exceeds MaxBones (" + std::to_string(RagdollComponent::MaxBones) + ")";
		std::unordered_set<std::string> joints;
		for (size_t i = 0; i < c.Bones.size(); i++)
		{
			const auto& bone = c.Bones[i];
			const std::string prefix = "Bones[" + std::to_string(i) + "].";
			if (bone.Joint.empty() || !joints.insert(bone.Joint).second)
				return prefix + "Joint must be nonempty and unique";
			if (!range(bone.Radius, 0, 100) || !range(bone.Length, 0, 100))
				return prefix + "Radius and Length must be finite and in 0..100 joint-local units";
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
