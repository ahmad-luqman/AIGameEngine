#include <doctest/doctest.h>

#include "Basalt/Renderer/TemporalHistory.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <limits>

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
	history.Commit();
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
	history.Commit();
	begin();
	CHECK(history.IsValid());
}

TEST_CASE("Temporal history keeps ordinary camera motion")
{
	TemporalHistory history;
	glm::mat4 view(1);
	history.Begin(true, 1, 2, { 64, 64 }, view, glm::mat4(1));
	history.Commit();
	view[3][0] = .2f;
	history.Begin(true, 1, 2, { 64, 64 }, view, glm::mat4(1));
	CHECK(history.IsValid());
	CHECK(history.GetPreviousViewProjection() == JitterProjection(glm::mat4(1), TemporalJitter(0), { 64, 64 }));
}

TEST_CASE("Sky reprojection preserves perspective and orthographic history through translation and rotation")
{
	const glm::uvec2 size(96, 64);
	for (const auto& projection : std::array{ glm::perspective(.9f, 1.5f, .1f, 100.0f), glm::ortho(-3.0f, 3.0f, -2.0f, 2.0f, .1f, 100.0f) })
	{
		TemporalHistory history;
		const glm::mat4 previousView = glm::lookAt(glm::vec3(2, 1, 3), glm::vec3(2, 1, 2), glm::vec3(0, 1, 0));
		const glm::mat4 previousProjection = JitterProjection(projection, TemporalJitter(0), size);
		history.Begin(true, 1, 2, size, previousView, projection);
		history.Commit();
		for (float angle : { 0.0f, .2f })
		{
			const glm::vec3 position(3, 2, 4);
			const glm::vec3 forward(std::sin(angle), 0, -std::cos(angle));
			const glm::mat4 view = glm::lookAt(position, position + forward, glm::vec3(0, 1, 0));
			history.Begin(true, 1, 2, size, view, projection);
			REQUIRE(history.IsValid());
			const glm::mat4 currentProjection = JitterProjection(projection, TemporalJitter(1), size);
			for (glm::vec2 ndc : { glm::vec2(0), glm::vec2(.4f, -.3f), glm::vec2(-.7f, .5f) })
			{
				const glm::vec4 currentClip(ndc, 1, 1);
				const glm::vec4 viewPoint = glm::inverse(currentProjection) * currentClip;
				const glm::vec3 worldPoint = glm::inverse(glm::mat3(view)) * (glm::vec3(viewPoint) / viewPoint.w);
				const glm::vec4 expected = previousProjection * glm::vec4(glm::mat3(previousView) * worldPoint, 1);
				const glm::vec4 actual = history.GetSkyReprojection() * currentClip;
				REQUIRE(actual.w > 0);
				CHECK(actual.x / actual.w == doctest::Approx(expected.x / expected.w).epsilon(.0001));
				CHECK(actual.y / actual.w == doctest::Approx(expected.y / expected.w).epsilon(.0001));
			}
		}
	}
}

TEST_CASE("Temporal camera identity and invalid views reset recoverable history")
{
	TemporalHistory history;
	auto begin = [&](const glm::mat4& view, uint64_t identity = 1) { history.Begin(true, 1, 2, { 64, 64 }, view, glm::mat4(1), identity); };
	begin(glm::mat4(1));
	history.Commit();
	SUBCASE("Nearby camera switch")
	{
		begin(glm::mat4(1), 2);
		CHECK_FALSE(history.IsValid());
	}
	SUBCASE("Nonfinite view")
	{
		glm::mat4 invalid(1);
		invalid[0][0] = std::numeric_limits<float>::quiet_NaN();
		begin(invalid);
		CHECK_FALSE(history.IsValid());
		history.Commit();
		begin(glm::mat4(1));
		CHECK_FALSE(history.IsValid());
	}
	SUBCASE("Singular view")
	{
		begin(glm::mat4(0));
		CHECK_FALSE(history.IsValid());
		history.Commit();
		begin(glm::mat4(1));
		CHECK_FALSE(history.IsValid());
	}
}

TEST_CASE("Temporal color-grid correction cancels raster jitter in both UV axes")
{
	TemporalHistory history;
	const glm::uvec2 size(96, 64);
	const glm::vec4 point(.2f, -.3f, .5f, 1);
	auto uv = [](glm::vec4 clip) { return glm::vec2(clip) / clip.w * glm::vec2(.5f, -.5f) + .5f; };
	history.Begin(true, 1, 2, size, glm::mat4(1), glm::mat4(1));
	auto previous = uv(history.GetJitteredProjection() * point);
	history.Commit();
	for (uint32_t frame = 1; frame <= TemporalJitterPeriod * 2; frame++)
	{
		history.Begin(true, 1, 2, size, glm::mat4(1), glm::mat4(1));
		auto current = uv(history.GetJitteredProjection() * point);
		auto corrected = previous + history.GetJitterDelta();
		CHECK(corrected.x == doctest::Approx(current.x));
		CHECK(corrected.y == doctest::Approx(current.y));
		previous = current;
		history.Commit();
	}
}
