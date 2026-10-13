#include "Basalt/Audio/AudioRules.h"

#include "Basalt/Core/Log.h"

#include <cmath>

namespace Basalt {

	namespace {

		bool IsFinitePositive(float value)
		{
			return std::isfinite(value) && value > 0.0f;
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
		return {};
	}

}
