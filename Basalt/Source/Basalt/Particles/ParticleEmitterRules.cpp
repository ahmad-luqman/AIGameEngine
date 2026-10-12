#include "Basalt/Particles/ParticleEmitterRules.h"

#include "Basalt/Core/Log.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Basalt {

	namespace {

		using Limits = ParticleEmitterComponent;
		constexpr float Unbounded = std::numeric_limits<float>::max();

		// One rule per field: the range its values must lie in. NaN lies in no range.
		struct FloatRule
		{
			const char* Name;
			float ParticleEmitterComponent::* Field;
			float Min;
			float Max;
		};

		constexpr FloatRule FloatRules[] = {
			{ "Duration", &ParticleEmitterComponent::Duration, Limits::MinTime, Unbounded },
			{ "Rate", &ParticleEmitterComponent::Rate, 0.0f, Limits::MaxRate },
			{ "ShapeRadius", &ParticleEmitterComponent::ShapeRadius, 0.0f, Limits::MaxMagnitude },
			{ "ConeAngle", &ParticleEmitterComponent::ConeAngle, 0.0f, Limits::MaxConeAngle },
			{ "LifetimeMin", &ParticleEmitterComponent::LifetimeMin, Limits::MinTime, Unbounded },
			{ "LifetimeMax", &ParticleEmitterComponent::LifetimeMax, Limits::MinTime, Unbounded },
			{ "SpeedMin", &ParticleEmitterComponent::SpeedMin, -Limits::MaxMagnitude, Limits::MaxMagnitude },
			{ "SpeedMax", &ParticleEmitterComponent::SpeedMax, -Limits::MaxMagnitude, Limits::MaxMagnitude },
			{ "GravityScale", &ParticleEmitterComponent::GravityScale, -Limits::MaxMagnitude, Limits::MaxMagnitude },
			{ "Drag", &ParticleEmitterComponent::Drag, 0.0f, Limits::MaxMagnitude },
			{ "StartSize", &ParticleEmitterComponent::StartSize, 0.0f, Limits::MaxMagnitude },
			{ "EndSize", &ParticleEmitterComponent::EndSize, 0.0f, Limits::MaxMagnitude },
		};

		struct Vec3Rule
		{
			const char* Name;
			glm::vec3 ParticleEmitterComponent::* Field;
			float Min;
			float Max;
		};

		constexpr Vec3Rule Vec3Rules[] = {
			{ "BoxExtents", &ParticleEmitterComponent::BoxExtents, 0.0f, Limits::MaxMagnitude },
			{ "Velocity", &ParticleEmitterComponent::Velocity, -Limits::MaxMagnitude, Limits::MaxMagnitude },
		};

		// Colors: HDR channels, opacity 0..1.
		struct ColorRule
		{
			const char* Name;
			glm::vec4 ParticleEmitterComponent::* Field;
		};

		constexpr ColorRule ColorRules[] = {
			{ "StartColor", &ParticleEmitterComponent::StartColor },
			{ "EndColor", &ParticleEmitterComponent::EndColor },
		};

		bool InRange(float value, float min, float max)
		{
			return value >= min && value <= max;
		}

		float ClampOr(float value, float min, float max, float fallback)
		{
			return std::isfinite(value) ? std::clamp(value, min, max) : fallback;
		}

		std::string RangeError(const char* name, float value, float min, float max)
		{
			if (max == Unbounded)
				return fmt::format("{} must be at least {} (got {})", name, min, value);
			return fmt::format("{} must be between {} and {} (got {})", name, min, max, value);
		}

		template<typename E>
		bool IsValidEnum(E value, E last)
		{
			return static_cast<uint32_t>(value) <= static_cast<uint32_t>(last);
		}

	}

	std::string ValidateParticleEmitter(const ParticleEmitterComponent& emitter)
	{
		if (emitter.MaxParticles < 1 || emitter.MaxParticles > Limits::MaxParticlesLimit)
			return fmt::format("MaxParticles must be between 1 and {} (got {})", Limits::MaxParticlesLimit, emitter.MaxParticles);
		if (emitter.BurstCount > Limits::MaxParticlesLimit)
			return fmt::format("BurstCount must be at most {} (got {})", Limits::MaxParticlesLimit, emitter.BurstCount);
		for (const FloatRule& rule : FloatRules)
		{
			if (!InRange(emitter.*rule.Field, rule.Min, rule.Max))
				return RangeError(rule.Name, emitter.*rule.Field, rule.Min, rule.Max);
		}
		for (const Vec3Rule& rule : Vec3Rules)
		{
			const glm::vec3& value = emitter.*rule.Field;
			for (int axis = 0; axis < 3; axis++)
			{
				if (!InRange(value[axis], rule.Min, rule.Max))
					return fmt::format("{} components must be between {} and {} (got [{}, {}, {}])", rule.Name, rule.Min, rule.Max, value.x, value.y, value.z);
			}
		}
		for (const ColorRule& rule : ColorRules)
		{
			const glm::vec4& color = emitter.*rule.Field;
			if (!InRange(color.r, 0.0f, Limits::MaxMagnitude) || !InRange(color.g, 0.0f, Limits::MaxMagnitude) || !InRange(color.b, 0.0f, Limits::MaxMagnitude) || !InRange(color.a, 0.0f, 1.0f))
				return fmt::format("{} needs RGB between 0 and {} and alpha between 0 and 1 (got [{}, {}, {}, {}])", rule.Name, Limits::MaxMagnitude, color.r, color.g, color.b, color.a);
		}
		// Only C++ code can write these; files and Lua go through the enum names.
		if (!IsValidEnum(emitter.Shape, ParticleEmitterShape::Cone) || !IsValidEnum(emitter.SimulationSpace, ParticleSimulationSpace::Local) || !IsValidEnum(emitter.Blend, ParticleBlendMode::Additive))
			return "Shape, SimulationSpace or Blend has a value outside its enum";
		return {};
	}

	ParticleEmitterComponent SanitizeParticleEmitter(ParticleEmitterComponent emitter)
	{
		const ParticleEmitterComponent defaults;
		emitter.MaxParticles = std::clamp<uint32_t>(emitter.MaxParticles, 1, Limits::MaxParticlesLimit);
		emitter.BurstCount = std::min(emitter.BurstCount, Limits::MaxParticlesLimit);
		for (const FloatRule& rule : FloatRules)
			emitter.*rule.Field = ClampOr(emitter.*rule.Field, rule.Min, rule.Max, defaults.*rule.Field);
		for (const Vec3Rule& rule : Vec3Rules)
		{
			for (int axis = 0; axis < 3; axis++)
				(emitter.*rule.Field)[axis] = ClampOr((emitter.*rule.Field)[axis], rule.Min, rule.Max, (defaults.*rule.Field)[axis]);
		}
		for (const ColorRule& rule : ColorRules)
		{
			glm::vec4& color = emitter.*rule.Field;
			for (int channel = 0; channel < 3; channel++)
				color[channel] = ClampOr(color[channel], 0.0f, Limits::MaxMagnitude, (defaults.*rule.Field)[channel]);
			color.a = ClampOr(color.a, 0.0f, 1.0f, (defaults.*rule.Field).a);
		}
		if (!IsValidEnum(emitter.Shape, ParticleEmitterShape::Cone))
			emitter.Shape = defaults.Shape;
		if (!IsValidEnum(emitter.SimulationSpace, ParticleSimulationSpace::Local))
			emitter.SimulationSpace = defaults.SimulationSpace;
		if (!IsValidEnum(emitter.Blend, ParticleBlendMode::Additive))
			emitter.Blend = defaults.Blend;
		return emitter;
	}

}
