#pragma once

#include <cstddef>
#include <iterator>

namespace Basalt {

	// Mixer buses. Music, SFX and UI feed Master, so a settings menu can scale one category or everything.
	// AudioBusNames is indexed by enum value (keep the enum and the names in the same order, and the count equal
	// to both); the component registry and the Lua Audio.* functions share it.
	enum class AudioBus
	{
		Master = 0,
		Music,
		SFX,
		UI
	};
	constexpr size_t AudioBusCount = 4;
	inline constexpr const char* AudioBusNames[] = { "Master", "Music", "SFX", "UI" };
	static_assert(std::size(AudioBusNames) == AudioBusCount);

}
