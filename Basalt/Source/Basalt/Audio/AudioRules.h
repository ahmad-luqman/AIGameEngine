#pragma once

#include "Basalt/Audio/AudioReverbSettings.h"
#include "Basalt/Scene/Components.h"

#include <string>

namespace Basalt {

	// The value rules of AudioSourceComponent, bus volumes and reverb settings, in one place for everything that
	// applies them: the component registry rejects sources outside them (files, Lua, automation, inspector), the
	// AudioSystem refuses to play a source that C++ code made invalid (and turns off live effects made invalid
	// while it plays), and the Lua Audio functions check volumes and reverb settings with them. A NaN gain would poison the whole output (its bus, Master and every other sound); a
	// negative one would silently invert the sound's phase.

	// The first rule `source` breaks, naming the field and its value; empty when it is valid.
	std::string ValidateAudioSource(const AudioSourceComponent& source);

	// A linear gain: finite and not negative (above 1 amplifies).
	bool IsValidAudioVolume(float volume);

	// The first effect field (Doppler, LowPass, ReverbSend) out of its range, naming it and its value; empty when
	// all are valid. Part of ValidateAudioSource; the AudioSystem also checks the effects alone, as they apply live.
	std::string ValidateAudioEffects(const AudioSourceComponent& source);

	// The first setting out of its 0..1 range, naming it and its value; empty when every one is valid.
	std::string ValidateAudioReverb(const AudioReverbSettings& reverb);

	// Finite and within [min, max].
	bool IsFiniteInRange(float value, float min, float max);

	// The speed of sound miniaudio's Doppler shift uses (MA_DEFAULT_SPEED_OF_SOUND, in world units per second).
	constexpr float AudioSpeedOfSound = 343.3f;
	// Range of AudioSourceComponent::Doppler. With the AudioSystem's speed limit (MaxDopplerSpeed) the pitch
	// stays between about 0.26 and 3.8: near the speed of sound miniaudio's pitch goes to 0 (silence) for a receding
	// listener and to infinity for an approaching source.
	constexpr float MaxAudioDoppler = 2.0f;
	// Range of a non-zero AudioSourceComponent::LowPass in Hz. The AudioSystem also keeps the cutoff at most
	// MaxAudioLowPassRateFraction times the output's sample rate (under its Nyquist frequency, half the rate).
	constexpr float MinAudioLowPass = 10.0f;
	// The top of human hearing.
	constexpr float MaxAudioLowPass = 20000.0f;
	constexpr float MaxAudioLowPassRateFraction = 0.45f;

	inline bool IsValidAudioDoppler(float doppler)
	{
		return IsFiniteInRange(doppler, 0.0f, MaxAudioDoppler);
	}
	// 0 (off) or a cutoff in range.
	inline bool IsValidAudioLowPass(float cutoff)
	{
		return cutoff == 0.0f || IsFiniteInRange(cutoff, MinAudioLowPass, MaxAudioLowPass);
	}
	inline bool IsValidAudioReverbSend(float send)
	{
		return IsFiniteInRange(send, 0.0f, 1.0f);
	}

}
