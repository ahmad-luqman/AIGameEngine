#include "Basalt/Physics/RagdollRules.h"

#include "Basalt/Animation/AnimationSystem.h"
#include "Basalt/Math/Math.h"
#include "Basalt/Physics/JoltUtils.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
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

	bool RagdollNeedsRebuild(const RagdollComponent& built, const RagdollComponent& current)
	{
		RagdollComponent structural = current;
		structural.Enabled = built.Enabled;
		structural.RecoveryTime = built.RecoveryTime;
		structural.MotorStrength = built.MotorStrength;
		structural.MotorFrequency = built.MotorFrequency;
		structural.MotorDamping = built.MotorDamping;
		structural.MotorTorque = built.MotorTorque;
		structural.BalanceStrength = built.BalanceStrength;
		structural.BalanceFrequency = built.BalanceFrequency;
		structural.BalanceDamping = built.BalanceDamping;
		structural.BalanceMaxTilt = built.BalanceMaxTilt;
		// Bones are matched by position: a list that gained, lost or reordered entries differs anyway.
		for (size_t i = 0; i < std::min(structural.Bones.size(), built.Bones.size()); i++)
			structural.Bones[i].MotorStrength = built.Bones[i].MotorStrength;
		return structural != built;
	}

	bool IsRagdollTeleport(const glm::vec3& offset, const glm::quat& to, const glm::quat& from, float seconds, float fixedStep)
	{
		const double window = std::max(static_cast<double>(seconds), static_cast<double>(fixedStep));
		// |cos(half the turn)| at a quarter turn.
		constexpr float QuarterTurn = 0.70710678f;
		return static_cast<double>(glm::length(offset)) > AnimationSystem::TeleportSpeed * window || std::abs(glm::dot(to, from)) < QuarterTurn;
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
		if (!range(c.MotorStrength, 0, 1))
			return "MotorStrength must be finite and in 0..1";
		if (!range(c.MotorFrequency, 0.1f, RagdollComponent::MaxMotorFrequency))
			return fmt::format("MotorFrequency must be finite and in 0.1..{} Hz", RagdollComponent::MaxMotorFrequency);
		if (!range(c.MotorDamping, 0, RagdollComponent::MaxMotorDamping))
			return fmt::format("MotorDamping must be finite and in 0..{}", RagdollComponent::MaxMotorDamping);
		if (!range(c.MotorTorque, 0, RagdollComponent::MaxMotorTorque))
			return fmt::format("MotorTorque must be finite and in 0..{} N·m", RagdollComponent::MaxMotorTorque);
		if (!range(c.BalanceStrength, 0, 1))
			return "BalanceStrength must be finite and in 0..1";
		if (!range(c.BalanceFrequency, 0.1f, RagdollComponent::MaxMotorFrequency))
			return fmt::format("BalanceFrequency must be finite and in 0.1..{} Hz", RagdollComponent::MaxMotorFrequency);
		if (!range(c.BalanceDamping, 0, RagdollComponent::MaxMotorDamping))
			return fmt::format("BalanceDamping must be finite and in 0..{}", RagdollComponent::MaxMotorDamping);
		if (!range(c.BalanceMaxTilt, 0, RagdollComponent::MaxTilt))
			return fmt::format("BalanceMaxTilt must be finite and in 0..{} degrees", RagdollComponent::MaxTilt);
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
			if (!range(bone.MotorStrength, 0, 1))
				return prefix + "MotorStrength must be finite and in 0..1";
		}
		return {};
	}

}
