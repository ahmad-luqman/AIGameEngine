#include <doctest/doctest.h>

#include "Basalt/Renderer/TemporalHistory.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>

using namespace Basalt;

TEST_CASE("Temporal jitter repeats without leaving a pixel and shifts both projection types correctly")
{
	CHECK(TemporalJitter(0).x == doctest::Approx(0));
	CHECK(TemporalJitter(0).y == doctest::Approx(-1.0f / 6));
	for (uint32_t i = 0; i < 16; i++)
	{
		const glm::vec2 jitter = TemporalJitter(i);
		CHECK(jitter == TemporalJitter(i + 8));
		CHECK(std::abs(jitter.x) <= .5f);
		CHECK(std::abs(jitter.y) <= .5f);
	}
	for (const auto& projection : std::array{ glm::perspective(1.0f, 1.5f, .1f, 100.0f), glm::ortho(-3.0f, 3.0f, -2.0f, 2.0f, .1f, 100.0f) })
		for (float z : { -1.0f, -5.0f, -50.0f })
		{
			const glm::vec4 point(.2f, -.5f, z, 1);
			const auto original = projection * point;
			const auto shifted = JitterProjection(projection, { .25f, -.4f }, { 100, 80 }) * point;
			CHECK(shifted.x / shifted.w - original.x / original.w == doctest::Approx(.005f));
			CHECK(shifted.y / shifted.w - original.y / original.w == doctest::Approx(-.01f));
			CHECK(shifted.z == original.z);
			CHECK(shifted.w == original.w);
			CHECK(JitterProjection(projection, { 1, 1 }, { 0, 0 }) == projection);
		}
}

TEST_CASE("Temporal history advances only on commit and resets at each view boundary")
{
	TemporalHistory history;
	glm::mat4 view(1), projection(1);
	uint64_t revision = 1, component = 2;
	glm::uvec2 size(64);
	auto begin = [&](bool enabled = true) { history.Begin(enabled, revision, component, size, view, projection); };
	begin();
	CHECK_FALSE(history.IsValid());
	CHECK(history.GetFrame() == 0);
	const auto previous = JitterProjection(projection, TemporalJitter(0), size) * view;
	history.Commit(previous);
	begin();
	CHECK(history.IsValid());
	CHECK(history.GetFrame() == 1);
	CHECK(history.GetPreviousViewProjection() == previous);
	begin();
	CHECK(history.GetFrame() == 1);
	SUBCASE("Scene replacement")
	{
		revision++;
	}
	SUBCASE("Post-process selection")
	{
		component++;
	}
	SUBCASE("Render area changed")
	{
		size.x++;
	}
	SUBCASE("Projection changed")
	{
		projection[0][0] = 2;
	}
	SUBCASE("Camera teleported")
	{
		view[3][0] = 20;
	}
	SUBCASE("Camera turned abruptly")
	{
		view = glm::rotate(view, 2.0f, glm::vec3(0, 1, 0));
	}
	SUBCASE("Explicit cut or failure")
	{
		history.Reset();
	}
	SUBCASE("Disabled, debug view, then re-enabled")
	{
		begin(false);
	}
	begin();
	CHECK_FALSE(history.IsValid());
	CHECK(history.GetFrame() == 0);
	history.Commit(projection * view);
	begin();
	CHECK(history.IsValid());
}

TEST_CASE("Temporal history keeps ordinary camera motion")
{
	TemporalHistory history;
	glm::mat4 view(1);
	history.Begin(true, 1, 2, { 64, 64 }, view, glm::mat4(1));
	history.Commit(view);
	view[3][0] = .2f;
	history.Begin(true, 1, 2, { 64, 64 }, view, glm::mat4(1));
	CHECK(history.IsValid());
	CHECK(history.GetPreviousViewProjection() == glm::mat4(1));
}
