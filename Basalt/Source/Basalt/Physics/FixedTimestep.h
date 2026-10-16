#pragma once

namespace Basalt {

	// Default of ProjectConfig::FixedTimestep (60 Hz). Scene::GetFixedTimestep picks a scene's own step
	// (PhysicsSettings::FixedTimestep) or else the project's.
	inline constexpr float DefaultFixedTimestep = 1.0f / 60.0f;
	// Longer steps make contacts and joints unstable.
	inline constexpr float MaxFixedTimestep = 0.25f;

	// A usable step: in (0, MaxFixedTimestep] seconds (false for NaN).
	inline bool IsValidFixedTimestep(float seconds)
	{
		return seconds > 0.0f && seconds <= MaxFixedTimestep;
	}

}
