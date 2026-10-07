#pragma once

#include <cstdint>
#include <random>
#include <utility>

namespace Basalt {

	// Seeded pseudo-random numbers that are identical on every platform and compiler. std::mt19937's
	// output is fully specified, but the standard distributions (uniform_real_distribution, ...) are not,
	// so the conversions to floats and ranges are done here. Scripts, replays and golden images rely on it.
	class Random
	{
	public:
		static constexpr uint32_t DefaultSeed = 0x8A5A17u;

		explicit Random(uint32_t seed = DefaultSeed)
			: m_Engine(seed)
		{
		}

		void Seed(uint32_t seed) { m_Engine.seed(seed); }

		uint32_t NextUInt32() { return static_cast<uint32_t>(m_Engine()); }

		// Uniform in [0, 1), using the top 24 bits (a float's full mantissa precision).
		float Float() { return static_cast<float>(NextUInt32() >> 8) * (1.0f / 16777216.0f); }

		// Uniform in [min, max) (the bounds may be given in either order).
		float Range(float min, float max)
		{
			if (max < min)
				std::swap(min, max);
			// Two statements on purpose: a single a + b * c may be fused into an FMA on some targets
			// (ARM64) but not others, which changes the last bit.
			const float offset = (max - min) * Float();
			return min + offset;
		}

		// Uniform integer in [min, max] inclusive, without modulo bias.
		int64_t Int(int64_t min, int64_t max);

	private:
		std::mt19937 m_Engine;
	};

}
