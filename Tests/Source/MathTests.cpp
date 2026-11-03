#include <doctest/doctest.h>

#include <Basalt/Math/Math.h>
#include <Basalt/Math/Random.h>

#include <cstdint>
#include <limits>

using namespace Basalt;

TEST_SUITE("Random")
{
	// Known answers (cross-checked against an independent MT19937 implementation). If a platform disagrees,
	// replays, scripted games and golden images are not portable to it.
	TEST_CASE("Random produces the same sequence on every platform")
	{
		Random raw(7);
		CHECK(raw.NextUInt32() == 327741615u);
		CHECK(raw.NextUInt32() == 976413892u);
		CHECK(raw.NextUInt32() == 3349725721u);

		Random floats(7);
		CHECK(floats.Float() == 1280240.0f / 16777216.0f); // 327741615 >> 8
		CHECK(floats.Float() == 3814116.0f / 16777216.0f); // 976413892 >> 8

		Random ints(7);
		const int64_t expected[] = { 6, 7, 7, 2, 2, 5, 7, 2 };
		for (int64_t value : expected)
			CHECK(ints.Int(1, 7) == value);
	}

	TEST_CASE("Random ranges stay in bounds, accept reversed bounds and cover the full int64 range")
	{
		Random random(123);
		for (int i = 0; i < 1000; i++)
		{
			const float f = random.Float();
			CHECK((f >= 0.0f && f < 1.0f));
			const float r = random.Range(3.0f, -2.0f);
			CHECK((r >= -2.0f && r < 3.0f));
			const int64_t n = random.Int(5, -5);
			CHECK((n >= -5 && n <= 5));
			const int64_t wide = random.Int(-5000000000LL, 5000000000LL);
			CHECK((wide >= -5000000000LL && wide <= 5000000000LL));
		}
		CHECK(random.Int(9, 9) == 9);
		// The full range must not overflow the span computation.
		random.Int(std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());
	}

	TEST_CASE("Random integers are unbiased across a small range")
	{
		Random random(99);
		int counts[3] = {};
		for (int i = 0; i < 30000; i++)
			counts[random.Int(0, 2)]++;
		for (int count : counts)
			CHECK((count > 9500 && count < 10500));
	}
}

TEST_SUITE("Math")
{
	// Animation poses that drive physics (powered ragdoll motors) interpolate with Slerp, so it must round the same on
	// every platform: these exact values come from Jolt's portable trigonometry.
	TEST_CASE("Slerp takes the shortest arc, normalizes and returns its endpoints")
	{
		const glm::quat from(1, 0, 0, 0);
		const glm::quat to = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
		CHECK(Math::Slerp(from, to, 0) == from);
		const glm::quat end = Math::Slerp(from, to, 1);
		CHECK(glm::abs(glm::dot(end, to)) == doctest::Approx(1.0f));
		const glm::quat third = Math::Slerp(from, to, 1.0f / 3.0f);
		CHECK(glm::degrees(glm::angle(third)) == doctest::Approx(30.0f).epsilon(1e-4));
		CHECK(glm::length(third) == doctest::Approx(1.0f));
		// -to is the same rotation: the shortest arc still turns 30 degrees, not 150.
		CHECK(glm::degrees(glm::angle(Math::Slerp(from, -to, 1.0f / 3.0f))) == doctest::Approx(30.0f).epsilon(1e-4));
		// Unnormalized input comes out unit length.
		CHECK(glm::length(Math::Slerp(from * 2.0f, to * 3.0f, 0.5f)) == doctest::Approx(1.0f));
	}
}
