#include <doctest/doctest.h>

#include <Breccia/Math/Math.h>
#include <Breccia/Math/Random.h>

#include <glm/gtc/quaternion.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

using namespace Breccia;

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

	TEST_CASE("Atan2 and YawRotation round the same everywhere and agree with libm")
	{
		// Exact bit patterns: root motion reaches physics through these, so a platform that differs breaks replays.
		CHECK(std::bit_cast<uint32_t>(Math::Atan2(0.3f, 0.7f)) == 1053773308u);
		CHECK(std::bit_cast<uint32_t>(Math::Atan2(-0.9f, -0.2f)) == 3219459379u);
		CHECK(std::bit_cast<uint32_t>(Math::YawRotation(1.1f).y) == 1057345237u);
		CHECK(std::bit_cast<uint32_t>(Math::YawRotation(1.1f).w) == 1062878988u);
		for (int i = 0; i < 64; i++)
		{
			const float angle = static_cast<float>(i) * 0.1f - 3.1f;
			const float y = std::sin(angle) * 2.5f, x = std::cos(angle) * 2.5f;
			CHECK(Math::Atan2(y, x) == doctest::Approx(std::atan2(y, x)).scale(0).epsilon(2e-6));
			const glm::quat turn = Math::YawRotation(angle);
			const glm::quat expected = glm::angleAxis(angle, glm::vec3(0, 1, 0));
			CHECK(glm::length(glm::vec4(turn.x, turn.y, turn.z, turn.w) - glm::vec4(expected.x, expected.y, expected.z, expected.w)) < 2e-6f);
		}
		// atan2's convention at the origin, which Jolt's divides into a NaN.
		CHECK(Math::Atan2(0, 0) == 0);
	}
}

namespace {
	// Degrees between two orientations, measured so that it stays exact for tiny angles and float quaternions a few bits
	// from unit length (a dot product and acos would report 0.05 degrees for those).
	double AngleBetween(const glm::quat& a, const glm::quat& b)
	{
		glm::dvec4 first = glm::normalize(glm::dvec4(a.w, a.x, a.y, a.z));
		glm::dvec4 second = glm::normalize(glm::dvec4(b.w, b.x, b.y, b.z));
		if (glm::dot(first, second) < 0)
			second = -second;
		return glm::degrees(4 * std::asin(glm::length(first - second) / 2));
	}

	glm::quat FromDegrees(const glm::vec3& degrees)
	{
		return Math::QuatFromEuler(glm::radians(degrees));
	}

	glm::vec3 ToDegrees(const glm::quat& rotation)
	{
		return glm::degrees(Math::EulerFromQuat(rotation));
	}
}

TEST_SUITE("Math")
{
	TEST_CASE("Euler angles keep the orientation at and near gimbal lock")
	{
		std::mt19937 random(7);
		std::uniform_real_distribution<float> angle(-180.0f, 180.0f);
		// glm::eulerAngles was up to 180 degrees off at the lock itself and 2 degrees off 1e-4 degrees from it.
		for (const float away : { 0.0f, 1e-5f, 1e-4f, 1e-3f, 1e-2f, 0.1f, 1.0f, 3.0f, 8.0f, 20.0f, 45.0f })
			for (const float sign : { 1.0f, -1.0f })
			{
				double worst = 0;
				for (int i = 0; i < 20000; i++)
				{
					const glm::quat rotation = FromDegrees({ angle(random), sign * (90.0f - away), angle(random) });
					worst = std::max(worst, AngleBetween(rotation, FromDegrees(ToDegrees(rotation))));
				}
				CAPTURE(away);
				CAPTURE(sign);
				CHECK(worst < 1e-4);
			}
		// Any rotation, not only those built near the lock.
		for (int i = 0; i < 100000; i++)
		{
			const glm::quat rotation = FromDegrees({ angle(random), angle(random), angle(random) });
			REQUIRE(AngleBetween(rotation, FromDegrees(ToDegrees(rotation))) < 1e-4);
		}
	}

	TEST_CASE("Rotations at exact gimbal lock serialize in one canonical form that round-trips to the same bits")
	{
		std::mt19937 random(11);
		std::uniform_real_distribution<float> angle(-180.0f, 180.0f);
		for (int i = 0; i < 100000; i++)
		{
			const float yaw = i % 2 == 0 ? 90.0f : -90.0f;
			const glm::quat rotation = FromDegrees({ angle(random), yaw, angle(random) });
			const glm::vec3 first = ToDegrees(rotation);
			REQUIRE(first.y == yaw);
			REQUIRE(first.z == 0.0f);
			const glm::vec3 second = ToDegrees(FromDegrees(first));
			REQUIRE(second.x == first.x);
			REQUIRE(second.y == first.y);
			REQUIRE(second.z == first.z);
			// q and -q are one rotation: their text must not differ by a turn.
			const glm::vec3 negated = ToDegrees(-rotation);
			REQUIRE(negated.x == first.x);
			REQUIRE(negated.y == first.y);
		}
		// Pitch +-180 is one orientation, whose sign is rounding noise: it must not alternate from save to save.
		for (const float yaw : { 90.0f, -90.0f })
			for (int pitch = -180; pitch <= 180; pitch++)
				for (int roll = -180; roll <= 180; roll += 15)
				{
					CAPTURE(pitch);
					CAPTURE(roll);
					const glm::vec3 first = ToDegrees(FromDegrees({ static_cast<float>(pitch), yaw, static_cast<float>(roll) }));
					const glm::vec3 second = ToDegrees(FromDegrees(first));
					REQUIRE(second.x == first.x);
					REQUIRE(first.x > -180.0f);
				}
		// Rotations that reach the lock through float products are further from it than those built whole.
		for (int i = 0; i < 100000; i++)
		{
			const glm::quat rotation = glm::normalize(glm::angleAxis(glm::radians(angle(random)), glm::vec3(0, 0, 1)) * glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)) * glm::angleAxis(glm::radians(angle(random)), glm::vec3(1, 0, 0)));
			const glm::vec3 euler = ToDegrees(rotation);
			REQUIRE(euler.y == 90.0f);
			REQUIRE(euler.z == 0.0f);
			REQUIRE(AngleBetween(rotation, FromDegrees(euler)) < 2e-4);
		}
		// A chain of turns that add up to the lock leaves more rounding than one product: the threshold must hold for it.
		double largest = 0;
		for (int i = 0; i < 100000; i++)
		{
			const float first = std::uniform_real_distribution<float>(5.0f, 85.0f)(random);
			glm::quat rotation = glm::angleAxis(glm::radians(angle(random)), glm::vec3(0, 0, 1)) * glm::angleAxis(glm::radians(first), glm::vec3(0, 1, 0));
			rotation = glm::normalize(rotation * glm::angleAxis(glm::radians(90.0f - first), glm::vec3(0, 1, 0)) * glm::angleAxis(glm::radians(angle(random)), glm::vec3(1, 0, 0)));
			const glm::vec3 euler = ToDegrees(rotation);
			largest = std::max(largest, static_cast<double>(std::abs(euler.y) - 90.0f));
			if (euler.z != 0.0f)
				largest = 1;
		}
		// Rotations a few float steps from the lock must also count as it, or their roll is noise.
		CHECK(largest <= 1e-3);
		// Only pitch minus roll (plus roll, below) matters at the lock.
		CHECK(ToDegrees(FromDegrees({ 30.0f, 90.0f, 10.0f })).x == doctest::Approx(20.0f).epsilon(1e-4));
		CHECK(ToDegrees(FromDegrees({ 30.0f, -90.0f, 10.0f })).x == doctest::Approx(40.0f).epsilon(1e-4));
	}

	TEST_CASE("No rotation has no negative zeros, which saved files would show as -0.0")
	{
		// The last one's roll rounds to -0 only when the angle turns float.
		for (const glm::quat& rotation : { glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::quat(1.0f, -0.0f, -0.0f, -0.0f), glm::quat(-1.0f, 0.0f, 0.0f, 0.0f), glm::quat(1.0f, 1e-30f, -1e-30f, 0.0f) })
		{
			const glm::vec3 euler = Math::EulerFromQuat(rotation);
			for (int axis = 0; axis < 3; axis++)
				CHECK_FALSE((euler[axis] == 0 && std::signbit(euler[axis])));
		}
	}

	TEST_CASE("Euler angles of quaternions that are not unit length")
	{
		CHECK(Math::EulerFromQuat(glm::quat(0.0f, 0.0f, 0.0f, 0.0f)) == glm::vec3(0.0f));
		const glm::quat rotation = FromDegrees({ 10.0f, 20.0f, 30.0f });
		const glm::quat scaled(rotation.w * 3, rotation.x * 3, rotation.y * 3, rotation.z * 3);
		const glm::vec3 expected = Math::EulerFromQuat(rotation);
		const glm::vec3 actual = Math::EulerFromQuat(scaled);
		for (int axis = 0; axis < 3; axis++)
			CHECK(actual[axis] == doctest::Approx(expected[axis]).epsilon(1e-6));
	}
}
