#include <doctest/doctest.h>

#include <Basalt/Renderer/LightClusters.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>

using namespace Basalt;

TEST_SUITE("LightClusters")
{
	TEST_CASE("Depth partitions cover perspective and orthographic clip intervals")
	{
		for (bool orthographic : { false, true })
		{
			const auto depths = LightClusterDepths(orthographic ? -10.0f : 0.1f, 1000.0f, orthographic);
			CHECK(LightClusterBoundary(depths, 0) == doctest::Approx(depths.x));
			CHECK(LightClusterBoundary(depths, LightClusterDimensions.z) == doctest::Approx(depths.y));
			CHECK(LightClusterSlice(depths, depths.x - 1) == 0);
			CHECK(LightClusterSlice(depths, depths.y + 1) == LightClusterDimensions.z - 1);
			for (uint32_t z = 0; z < LightClusterDimensions.z; z++)
			{
				const float a = LightClusterBoundary(depths, z);
				const float b = LightClusterBoundary(depths, z + 1);
				CHECK(b > a);
				CHECK(LightClusterSlice(depths, (a + b) * 0.5f) == z);
			}
		}
	}

	TEST_CASE("Every cluster contains its corners with translated rotated and asymmetric cameras")
	{
		const glm::mat4 view = glm::scale(glm::lookAt(glm::vec3(10, -3, 7), glm::vec3(-1, 2, -4), glm::vec3(0, 1, 0)), glm::vec3(0.7f, 1.3f, 2.0f));
		// Use double precision for the oracle so far-plane unprojection does not move a corner outside.
		const glm::dmat4 inverseView = glm::inverse(glm::dmat4(view));
		for (bool orthographic : { false, true })
		{
			const auto depths = LightClusterDepths(orthographic ? -2.0f : 0.1f, 90.0f, orthographic);
			const glm::mat4 projection = orthographic ? glm::ortho(-4.0f, 6.0f, -3.0f, 5.0f, depths.x, depths.y) : glm::frustum(-0.08f, 0.12f, -0.06f, 0.09f, depths.x, depths.y);
			const glm::dmat4 inverseProjection = glm::inverse(glm::dmat4(projection));
			for (uint32_t z = 0; z < LightClusterDimensions.z; z++)
				for (uint32_t y = 0; y < LightClusterDimensions.y; y++)
					for (uint32_t x = 0; x < LightClusterDimensions.x; x++)
					{
						const auto planes = LightClusterPlanes(view, projection, depths, { x, y, z });
						for (uint32_t corner = 0; corner < 8; corner++)
						{
							const float depth = LightClusterBoundary(depths, z + (corner >> 2));
							const float nx = 2.0f * static_cast<float>(x + (corner & 1u)) / static_cast<float>(LightClusterDimensions.x) - 1.0f;
							const float ny = 1.0f - 2.0f * static_cast<float>(y + ((corner >> 1) & 1u)) / static_cast<float>(LightClusterDimensions.y);
							const glm::dvec4 clipDepth = glm::dmat4(projection) * glm::dvec4(0, 0, -depth, 1);
							glm::dvec4 point = inverseView * inverseProjection * glm::dvec4(nx, ny, clipDepth.z / clipDepth.w, 1);
							point /= point.w;
							REQUIRE(LightClusterIntersects(planes, glm::vec3(point), 0));
						}
					}
		}
	}

	TEST_CASE("Sphere overlap is conservative at planes but rejects distant lights")
	{
		const auto depths = LightClusterDepths(0.1f, 100.0f, false);
		const auto projection = glm::perspective(glm::radians(60.0f), 1.5f, depths.x, depths.y);
		const auto planes = LightClusterPlanes(glm::mat4(1), projection, depths, { 8, 4, 12 });
		CHECK_FALSE(LightClusterIntersects(planes, { 1000, 1000, 1000 }, 1));
		CHECK_FALSE(LightClusterIntersects(planes, { 0, 0, 10 }, 1));
		CHECK(LightClusterIntersects(planes, { 0, 0, 0 }, 100));
		// A point on a depth boundary belongs to both adjacent cells, including rounding slack.
		const float boundary = LightClusterBoundary(depths, 12);
		CHECK(LightClusterIntersects(planes, { 0, 0, -boundary }, 0));
		CHECK(LightClusterIntersects(LightClusterPlanes(glm::mat4(1), projection, depths, { 8, 4, 11 }), { 0, 0, -boundary }, 0));
	}
	TEST_CASE("Edge slices conservatively cover clamped depths and invalid ranges stay finite")
	{
		const auto depths = LightClusterDepths(1, 10, false);
		const auto projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
		CHECK(LightClusterIntersects(LightClusterPlanes(glm::mat4(1), projection, depths, { 8, 4, 0 }), { 0, 0, -0.2f }, 0));
		CHECK(LightClusterIntersects(LightClusterPlanes(glm::mat4(1), projection, depths, { 8, 4, 23 }), { 0, 0, -50 }, 0));
		for (glm::vec2 invalid : { glm::vec2(0, 10), { 10, 1 }, { 1, std::numeric_limits<float>::infinity() } })
		{
			const auto safe = LightClusterDepths(invalid.x, invalid.y, false);
			CHECK(safe.x > 0);
			CHECK(safe.y > safe.x);
			CHECK(std::isfinite(LightClusterBoundary(safe, 12)));
		}
		LightClusters uninitialized;
		CHECK_FALSE(uninitialized.IsValid());
		CHECK_FALSE(uninitialized.Build(nullptr, glm::mat4(1), projection, depths, 0));
	}

	TEST_CASE("Tiny lights are rejected by neighbouring x and y cells")
	{
		const auto depths = LightClusterDepths(0, 24, true);
		const auto projection = glm::ortho(-8.0f, 8.0f, -4.5f, 4.5f, 0.0f, 24.0f);
		for (uint32_t y = 0; y < LightClusterDimensions.y; y++)
			for (uint32_t x = 0; x < LightClusterDimensions.x; x++)
			{
				const auto planes = LightClusterPlanes(glm::mat4(1), projection, depths, { x, y, 4 });
				CHECK(LightClusterIntersects(planes, { 0.5f, 0.0f, -4.5f }, 0.1f) == (x == 8 && y == 4));
			}
		CHECK(LightClusterIndex({ 15, 8, 23 }) == LightClusterCount - 1);
	}
}
