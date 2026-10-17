#pragma once

#include <array>

namespace Basalt {

	// The room the reverb simulates (Freeverb's parameters, each 0..1). One setting for the whole play session:
	// every bus's reverb uses it, so a script changes it as the listener moves between spaces.
	struct AudioReverbSettings
	{
		// Larger rooms ring longer.
		float RoomSize = 0.5f;
		// How quickly high frequencies die away: soft furnishings damp, bare stone does not.
		float Damping = 0.25f;
		// Stereo spread of the reverb tail (0 = mono).
		float Width = 1.0f;

		bool operator==(const AudioReverbSettings&) const = default;
	};

	// Each setting by name, for the code that handles them by name (validation, Lua), in declaration order.
	struct AudioReverbField
	{
		const char* Name;
		float AudioReverbSettings::* Member;
	};
	inline constexpr auto AudioReverbFields = std::to_array<AudioReverbField>({
		{ "RoomSize", &AudioReverbSettings::RoomSize },
		{ "Damping", &AudioReverbSettings::Damping },
		{ "Width", &AudioReverbSettings::Width },
	});
	static_assert(sizeof(AudioReverbSettings) == AudioReverbFields.size() * sizeof(float), "every reverb setting needs an AudioReverbFields entry");

}
