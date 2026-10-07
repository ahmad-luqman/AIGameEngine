#include "Basalt/Math/Random.h"

#include <limits>
#include <utility>

namespace Basalt {

	int64_t Random::Int(int64_t min, int64_t max)
	{
		if (max < min)
			std::swap(min, max);

		// Work in unsigned arithmetic: max - min overflows int64_t for wide ranges.
		const uint64_t span = static_cast<uint64_t>(max) - static_cast<uint64_t>(min);
		auto next64 = [this]() { return (static_cast<uint64_t>(NextUInt32()) << 32) | NextUInt32(); };
		if (span == std::numeric_limits<uint64_t>::max())
			return static_cast<int64_t>(next64());

		const uint64_t count = span + 1;
		if (count <= std::numeric_limits<uint32_t>::max())
		{
			// Rejection sampling on 32-bit draws: reject the top partial bucket so every value is equally likely.
			const uint32_t count32 = static_cast<uint32_t>(count);
			const uint32_t limit = std::numeric_limits<uint32_t>::max() - (std::numeric_limits<uint32_t>::max() % count32 + 1) % count32;
			uint32_t value = NextUInt32();
			while (value > limit)
				value = NextUInt32();
			return static_cast<int64_t>(static_cast<uint64_t>(min) + value % count32);
		}

		const uint64_t limit = std::numeric_limits<uint64_t>::max() - (std::numeric_limits<uint64_t>::max() % count + 1) % count;
		uint64_t value = next64();
		while (value > limit)
			value = next64();
		return static_cast<int64_t>(static_cast<uint64_t>(min) + value % count);
	}

}
