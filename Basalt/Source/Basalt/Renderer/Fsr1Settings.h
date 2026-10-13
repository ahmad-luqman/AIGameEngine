#pragma once

#include <algorithm>
#include <cmath>
#include <string>

namespace Basalt {

	inline constexpr float MinFsr1Sharpness = 0.0f;
	inline constexpr float MaxFsr1Sharpness = 2.0f;
	inline constexpr float DefaultFsr1Sharpness = 0.2f;

	// Direct C++ callers bypass the strict scene reader; keep their values safe for RCAS too.
	inline float SanitizeFsr1Sharpness(float sharpness)
	{
		return std::isfinite(sharpness) ? std::clamp(sharpness, MinFsr1Sharpness, MaxFsr1Sharpness) : DefaultFsr1Sharpness;
	}

	inline std::string Fsr1SharpnessRange()
	{
		return "[" + std::to_string(static_cast<int>(MinFsr1Sharpness)) + ", " + std::to_string(static_cast<int>(MaxFsr1Sharpness)) + "] stops (0 = strongest)";
	}

}
