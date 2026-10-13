#pragma once

#include "Basalt/Scene/Components.h"

#include <string>

namespace Basalt {

	// The value rules of AudioSourceComponent and of bus volumes, in one place for everything that applies
	// them: the component registry rejects sources outside them (files, Lua, automation, inspector), the
	// AudioSystem refuses to play a source that C++ code made invalid, and the Lua Audio functions check
	// volumes with them. A NaN gain would poison the whole output (its bus, Master and every other sound); a
	// negative one would silently invert the sound's phase.

	// The first rule `source` breaks, naming the field and its value; empty when it is valid.
	std::string ValidateAudioSource(const AudioSourceComponent& source);

	// A linear gain: finite and not negative (above 1 amplifies).
	bool IsValidAudioVolume(float volume);

}
