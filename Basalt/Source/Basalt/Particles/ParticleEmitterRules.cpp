#include "Basalt/Particles/ParticleEmitterRules.h"

#include "Basalt/Core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Basalt {

	namespace {

		using Emitter = ParticleEmitterComponent;

		struct FloatRule
		{
			const char* Name;
			float Emitter::* Field;
			float Min;
			float Max;
		};

		constexpr FloatRule FloatRules[] = {
			{ "Duration", &Emitter::Duration, Emitter::MinTime, Emitter::MaxTime },
			{ "Rate", &Emitter::Rate, 0.0f, Emitter::MaxRate },
			{ "ShapeRadius", &Emitter::ShapeRadius, 0.0f, Emitter::MaxMagnitude },
			{ "ConeAngle", &Emitter::ConeAngle, 0.0f, Emitter::MaxConeAngle },
			{ "LifetimeMin", &Emitter::LifetimeMin, Emitter::MinTime, Emitter::MaxTime },
			{ "LifetimeMax", &Emitter::LifetimeMax, Emitter::MinTime, Emitter::MaxTime },
			{ "SpeedMin", &Emitter::SpeedMin, -Emitter::MaxMagnitude, Emitter::MaxMagnitude },
			{ "SpeedMax", &Emitter::SpeedMax, -Emitter::MaxMagnitude, Emitter::MaxMagnitude },
			{ "GravityScale", &Emitter::GravityScale, -Emitter::MaxMagnitude, Emitter::MaxMagnitude },
			{ "Drag", &Emitter::Drag, 0.0f, Emitter::MaxMagnitude },
			{ "StartSize", &Emitter::StartSize, 0.0f, Emitter::MaxMagnitude },
			{ "EndSize", &Emitter::EndSize, 0.0f, Emitter::MaxMagnitude },
		};

		struct Vec3Rule
		{
			const char* Name;
			glm::vec3 Emitter::* Field;
			float Min;
			float Max;
		};

		constexpr Vec3Rule Vec3Rules[] = {
			{ "BoxExtents", &Emitter::BoxExtents, 0.0f, Emitter::MaxMagnitude },
			{ "Velocity", &Emitter::Velocity, -Emitter::MaxMagnitude, Emitter::MaxMagnitude },
		};

		// HDR color channels, opacity 0..1.
		struct ColorRule
		{
			const char* Name;
			glm::vec4 Emitter::* Field;
			float RgbMax;
			float AlphaMax;
		};

		constexpr ColorRule ColorRules[] = {
			{ "StartColor", &Emitter::StartColor, Emitter::MaxMagnitude, 1.0f },
			{ "EndColor", &Emitter::EndColor, Emitter::MaxMagnitude, 1.0f },
		};

		constexpr const char* AxisNames[] = { ".x", ".y", ".z", ".a" };
		constexpr const char* ChannelNames[] = { ".r", ".g", ".b", ".a" };

		// Walks every rule once. Each field gets the value it must have (clamped, or the default when it is
		// not finite or not an enum value); a field whose value differs breaks its rule and is reported to
		// `onProblem` and, when `repair`, replaced. NaN differs from everything, -0.0 equals 0.0.
		template<typename OnProblem>
		void ApplyRules(Emitter& emitter, bool repair, OnProblem&& onProblem)
		{
			const Emitter defaults;
			auto checkFloat = [&](float& value, float min, float max, float fallback, const std::string& name) {
				const float required = std::isfinite(value) ? std::clamp(value, min, max) : fallback;
				if (required == value)
					return;
				onProblem(fmt::format("{} must be between {} and {} (got {})", name, min, max, value));
				if (repair)
					value = required;
			};
			auto checkCount = [&](uint32_t& value, uint32_t min, const char* name) {
				const uint32_t required = std::clamp(value, min, Emitter::MaxParticlesLimit);
				if (required == value)
					return;
				onProblem(fmt::format("{} must be between {} and {} (got {})", name, min, Emitter::MaxParticlesLimit, value));
				if (repair)
					value = required;
			};
			auto checkEnum = [&](auto& value, size_t count, auto fallback, const char* name) {
				if (static_cast<size_t>(value) < count)
					return;
				onProblem(fmt::format("{} must be one of its {} values (got {})", name, count, static_cast<size_t>(value)));
				if (repair)
					value = fallback;
			};

			checkCount(emitter.MaxParticles, 1, "MaxParticles");
			checkCount(emitter.BurstCount, 0, "BurstCount");
			for (const FloatRule& rule : FloatRules)
				checkFloat(emitter.*rule.Field, rule.Min, rule.Max, defaults.*rule.Field, rule.Name);
			for (const Vec3Rule& rule : Vec3Rules)
			{
				for (int axis = 0; axis < 3; axis++)
					checkFloat((emitter.*rule.Field)[axis], rule.Min, rule.Max, (defaults.*rule.Field)[axis], std::string(rule.Name) + AxisNames[axis]);
			}
			for (const ColorRule& rule : ColorRules)
			{
				for (int channel = 0; channel < 4; channel++)
					checkFloat((emitter.*rule.Field)[channel], 0.0f, channel == 3 ? rule.AlphaMax : rule.RgbMax, (defaults.*rule.Field)[channel], std::string(rule.Name) + ChannelNames[channel]);
			}
			// Only C++ code can write these out of range; files and Lua go through the enum names.
			checkEnum(emitter.Shape, ParticleEmitterShapeCount, defaults.Shape, "Shape");
			checkEnum(emitter.SimulationSpace, ParticleSimulationSpaceCount, defaults.SimulationSpace, "SimulationSpace");
			checkEnum(emitter.Blend, ParticleBlendModeCount, defaults.Blend, "Blend");
		}

	}

	std::vector<std::string> FindParticleEmitterProblems(const ParticleEmitterComponent& emitter)
	{
		std::vector<std::string> problems;
		ParticleEmitterComponent copy = emitter;
		ApplyRules(copy, false, [&](std::string problem) { problems.push_back(std::move(problem)); });
		return problems;
	}

	std::string ValidateParticleEmitter(const ParticleEmitterComponent& emitter)
	{
		std::vector<std::string> problems = FindParticleEmitterProblems(emitter);
		return problems.empty() ? std::string() : std::move(problems.front());
	}

	ParticleEmitterComponent SanitizeParticleEmitter(ParticleEmitterComponent emitter)
	{
		ApplyRules(emitter, true, [](const std::string&) {});
		return emitter;
	}

}
