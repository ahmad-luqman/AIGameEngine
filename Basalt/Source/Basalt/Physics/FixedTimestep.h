#pragma once

namespace Basalt {

	// Physics simulation step: the project's FixedTimestep, unless a scene overrides it (PhysicsSettings).
	inline constexpr float DefaultFixedTimestep = 1.0f / 60.0f;
	// Longer steps make contacts and joints unstable.
	inline constexpr float MaxFixedTimestep = 0.25f;

	// A usable step: in (0, MaxFixedTimestep] seconds (false for NaN).
	inline bool IsValidFixedTimestep(float seconds)
	{
		return seconds > 0.0f && seconds <= MaxFixedTimestep;
	}

}
