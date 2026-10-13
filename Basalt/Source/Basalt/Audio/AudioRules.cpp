#include "Basalt/Audio/AudioRules.h"

#include "Basalt/Core/Log.h"

#include <cmath>

namespace Basalt {

	bool IsValidAudioVolume(float volume)
	{
		return std::isfinite(volume) && volume >= 0.0f;
	}

	std::string ValidateAudioSource(const AudioSourceComponent& source)
	{
		if (!IsValidAudioVolume(source.Volume))
			return fmt::format("Volume must be a finite number >= 0 (got {})", source.Volume);
		// miniaudio ignores a pitch of 0 or less: the clip would silently play at its normal speed.
		if (!std::isfinite(source.Pitch) || source.Pitch <= 0.0f)
			return fmt::format("Pitch must be a finite number > 0 (got {})", source.Pitch);
		// The inverse-distance model divides by MinDistance: 0 would silence the source at every distance.
		if (!std::isfinite(source.MinDistance) || source.MinDistance <= 0.0f)
			return fmt::format("MinDistance must be a finite number > 0 (got {})", source.MinDistance);
		if (!std::isfinite(source.MaxDistance) || source.MaxDistance < source.MinDistance)
			return fmt::format("MaxDistance must be a finite number >= MinDistance (got MaxDistance {} and MinDistance {})", source.MaxDistance, source.MinDistance);
		if (static_cast<size_t>(source.Bus) >= AudioBusCount)
			return fmt::format("Bus must be one of its {} values (got {})", AudioBusCount, static_cast<size_t>(source.Bus));
		return {};
	}

}
