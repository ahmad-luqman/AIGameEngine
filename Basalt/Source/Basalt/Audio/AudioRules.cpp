#include "Basalt/Audio/AudioRules.h"

#include "Basalt/Core/Log.h"

#include <cmath>
#include <utility>

namespace Basalt {

	namespace {

		bool IsFinitePositive(float value)
		{
			return std::isfinite(value) && value > 0.0f;
		}

		bool IsInRange(float value, float min, float max)
		{
			return std::isfinite(value) && value >= min && value <= max;
		}

	}

	bool IsValidAudioVolume(float volume)
	{
		return std::isfinite(volume) && volume >= 0.0f;
	}

	std::string ValidateAudioSource(const AudioSourceComponent& source)
	{
		if (!IsValidAudioVolume(source.Volume))
			return fmt::format("Volume must be a finite number >= 0 (got {})", source.Volume);
		// miniaudio ignores a pitch of 0 or less: the clip would silently play at its normal speed.
		if (!IsFinitePositive(source.Pitch))
			return fmt::format("Pitch must be a finite number > 0 (got {})", source.Pitch);
		// MinDistance is the numerator of the inverse-distance gain (MinDistance / distance): 0 silences the source
		// at every distance, and gives 0 / 0 at distance 0.
		if (!IsFinitePositive(source.MinDistance))
			return fmt::format("MinDistance must be a finite number > 0 (got {})", source.MinDistance);
		if (!std::isfinite(source.MaxDistance) || source.MaxDistance < source.MinDistance)
			return fmt::format("MaxDistance must be a finite number >= MinDistance (got MaxDistance {} and MinDistance {})", source.MaxDistance, source.MinDistance);
		if (static_cast<size_t>(source.Bus) >= AudioBusCount)
			return fmt::format("Bus must be one of its {} values (got {})", AudioBusCount, static_cast<size_t>(source.Bus));
		if (!IsInRange(source.Doppler, 0.0f, MaxAudioDoppler))
			return fmt::format("Doppler must be a number from 0 to {} (got {})", MaxAudioDoppler, source.Doppler);
		if (source.LowPass != 0.0f && !IsInRange(source.LowPass, MinAudioLowPass, MaxAudioLowPass))
			return fmt::format("LowPass must be 0 (off) or a cutoff from {} to {} Hz (got {})", MinAudioLowPass, MaxAudioLowPass, source.LowPass);
		if (!IsInRange(source.ReverbSend, 0.0f, 1.0f))
			return fmt::format("ReverbSend must be a number from 0 to 1 (got {})", source.ReverbSend);
		return {};
	}

	std::string ValidateAudioReverb(const AudioReverbSettings& reverb)
	{
		const std::pair<const char*, float> settings[] = { { "RoomSize", reverb.RoomSize }, { "Damping", reverb.Damping }, { "Width", reverb.Width } };
		for (const auto& [name, value] : settings)
		{
			if (!IsInRange(value, 0.0f, 1.0f))
				return fmt::format("{} must be a number from 0 to 1 (got {})", name, value);
		}
		return {};
	}

}
