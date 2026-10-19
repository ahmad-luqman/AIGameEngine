#include "Basalt/Core/FrameTiming.h"

#include <doctest/doctest.h>

#include <limits>

using namespace Basalt;

TEST_CASE("Host frame timing averages real latency, including stalls, and evicts old samples")
{
	FrameTiming timing;
	CHECK(timing.GetMilliseconds() == 0);
	timing.Add(10);
	timing.Add(30);
	CHECK(timing.GetMilliseconds() == 20);
	for (float bad : { 0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
		timing.Add(bad);
	CHECK(timing.GetMilliseconds() == 20);
	for (size_t i = 0; i < FrameTiming::Window; ++i)
		timing.Add(1000);
	CHECK(timing.GetMilliseconds() == 1000);
	timing.Add(10);
	CHECK(timing.GetMilliseconds() == doctest::Approx((59 * 1000.0f + 10) / 60));
}
