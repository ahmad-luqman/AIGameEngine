#pragma once

#include "Basalt/Audio/AudioReverbSettings.h"
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

	// The first setting out of its 0..1 range, naming it and its value; empty when every one is valid.
	std::string ValidateAudioReverb(const AudioReverbSettings& reverb);

	// Range of AudioSourceComponent::Doppler. With the AudioSystem's speed limit (MaxDopplerSpeed) the pitch
	// stays between about 0.26 and 3.8: miniaudio would drop it to 0 (silence) for sources near the speed of sound.
	constexpr float MaxAudioDoppler = 2.0f;
	// Range of a non-zero AudioSourceComponent::LowPass in Hz (the top of human hearing; the AudioSystem also keeps
	// the cutoff below the output's Nyquist frequency).
	constexpr float MinAudioLowPass = 10.0f;
	constexpr float MaxAudioLowPass = 20000.0f;

}
