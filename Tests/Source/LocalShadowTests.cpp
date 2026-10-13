#include <doctest/doctest.h>

#include <Basalt/Renderer/LocalShadows.h>
#include <Basalt/Scene/ComponentRegistry.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>
#include <Basalt/Scene/SceneSerializer.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>

using namespace Basalt;

TEST_SUITE("Local shadows")
{
	const glm::mat4 View(1.0f);
	const glm::mat4 Projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);

	TEST_CASE("Atlas prioritizes projected influence and breaks ties by UUID regardless of request order")
	{
		std::vector<LocalShadowRequest> requests = {
			{ 3, 0, LocalShadowType::Spot, { 0, 0, -20 }, 1 },
			{ 2, 1, LocalShadowType::Spot, { 0, 0, -5 }, 1 },
			{ 1, 2, LocalShadowType::Spot, { 0, 0, -5 }, 1 }
		};
		auto plan = BuildLocalShadowPlan(requests, View, Projection, 2);
		REQUIRE(plan.Allocations.size() == 2);
		CHECK(plan.Allocations[0].LightIndex == 2);
		CHECK(plan.Allocations[1].LightIndex == 1);
		std::reverse(requests.begin(), requests.end());
		plan = BuildLocalShadowPlan(requests, View, Projection, 2);
		REQUIRE(plan.Allocations.size() == 2);
		CHECK(plan.Allocations[0].LightIndex == 2);
		CHECK(plan.Allocations[1].LightIndex == 1);
		requests[0].Position.z = -50;
		plan = BuildLocalShadowPlan(requests, View, Projection, 1);
		REQUIRE(plan.Allocations.size() == 1);
		CHECK(plan.Allocations[0].LightIndex == 1);
	}

	TEST_CASE("Screen-filling influences tie at one viewport in both camera projections")
	{
		for (bool orthographic : { false, true })
		{
			const auto projection = orthographic ? glm::ortho(-10.0f, 10.0f, -10.0f, 10.0f, 0.1f, 100.0f) : Projection;
			std::array requests = {
				LocalShadowRequest{ 1, 0, LocalShadowType::Point, { 0, 0, -4 }, orthographic ? 10.0f : 2.0f },
				LocalShadowRequest{ 2, 1, LocalShadowType::Point, { 0, 0, -4 }, orthographic ? 20.0f : 3.0f }
			};
			for (int order = 0; order < 2; order++)
			{
				const auto plan = BuildLocalShadowPlan(requests, View, projection, 6);
				REQUIRE(plan.Allocations.size() == 1);
				CHECK(plan.Allocations[0].LightIndex == 0);
				CHECK(plan.Skipped == 1);
				std::reverse(requests.begin(), requests.end());
			}
		}
	}

	TEST_CASE("Spot importance accounts for its cone and direction")
	{
		const std::array requests = {
			LocalShadowRequest{ 1, 0, LocalShadowType::Spot, { 0, 0, -10 }, 5, { 0, 0, -1 }, 5 },
			LocalShadowRequest{ 2, 1, LocalShadowType::Spot, { 0, 0, -10 }, 5, { 0, 0, -1 }, 60 }
		};
		const auto plan = BuildLocalShadowPlan(requests, View, Projection, 1);
		REQUIRE(plan.Allocations.size() == 1);
		CHECK(plan.Allocations[0].LightIndex == 1);
	}

	TEST_CASE("Atlas allocates whole cubes, fills remaining space with spots and bounds every tile")
	{
		std::vector<LocalShadowRequest> requests;
		requests.reserve(80);
		for (uint32_t i = 0; i < 80; i++)
			requests.push_back({ i + 1, i, i < 11 ? LocalShadowType::Point : LocalShadowType::Spot, { 0, 0, -5 }, 2 });
		const auto plan = BuildLocalShadowPlan(requests, View, Projection, 1000);
		REQUIRE(plan.Maps.size() == MaxLocalShadowMaps);
		REQUIRE(plan.Allocations.size() == 14);
		CHECK(plan.Skipped == 66);
		uint32_t next = 0;
		for (const auto& allocation : plan.Allocations)
		{
			CHECK(allocation.FirstMap == next);
			CHECK(allocation.MapCount == (allocation.LightIndex < 11 ? 6u : 1u));
			CHECK(allocation.LightIndex != 10); // The eleventh cube cannot fit in the last four tiles.
			next += allocation.MapCount;
		}
		for (uint32_t i = 0; i < plan.Maps.size(); i++)
		{
			const auto rect = plan.Maps[i].AtlasRect;
			CHECK(rect.x == doctest::Approx(0.125f));
			CHECK(rect.y == rect.x);
			CHECK(rect.z == doctest::Approx(static_cast<float>(i % 8) / 8.0f));
			const uint32_t row = i / 8;
			CHECK(rect.w == doctest::Approx(static_cast<float>(row) / 8.0f));
			CHECK(rect.z + rect.x <= 1.0f);
			CHECK(rect.w + rect.y <= 1.0f);
		}
		CHECK(BuildLocalShadowPlan(requests, View, Projection, 0).Maps.empty());
		CHECK(BuildLocalShadowPlan(std::span(requests).first(1), View, Projection, 5).Maps.empty());
		CHECK(BuildLocalShadowPlan({}, View, Projection).Maps.empty());
	}

	TEST_CASE("Visibility tests influence bounds, supports orthographic cameras and ignores invalid requests")
	{
		std::vector<LocalShadowRequest> requests = {
			{ 1, 0, LocalShadowType::Spot, { 100, 0, -5 }, 1 },
			{ 2, 1, LocalShadowType::Spot, { 0, 0, 5 }, 1 },
			{ 3, 2, LocalShadowType::Spot, { 0, 0, -200 }, 1 },
			{ 4, 3, LocalShadowType::Spot, { 4, 0, -5 }, 3 },
			{ 5, 4, LocalShadowType::Spot, { 0, 0, -5 }, 0 },
			{ 6, 5, LocalShadowType::Spot, { 0, 0, -5 }, 1, { 0, 0, 0 } },
			{ 7, 6, LocalShadowType::Spot, { 0, 0, -5 }, 1, { 0, 0, -1 }, 90 },
			{ 8, 7, LocalShadowType::Point, { 0, 0, -5 }, std::numeric_limits<float>::infinity() },
			{ 9, 8, LocalShadowType::Point, { std::numeric_limits<float>::quiet_NaN(), 0, -5 }, 1 },
			{ 10, 9, LocalShadowType::Point, { 0, 0, -5 }, -1 },
			{ 11, 10, LocalShadowType::Point, { 0, 0, -5 }, 0.0005f }
		};
		const auto plan = BuildLocalShadowPlan(requests, View, Projection);
		REQUIRE(plan.Allocations.size() == 1);
		CHECK(plan.Allocations[0].LightIndex == 3); // The light center is offscreen, but its receivers are not.
		requests = { { 2, 0, LocalShadowType::Spot, { 0, 0, -4 }, 1 }, { 1, 1, LocalShadowType::Spot, { 0, 0, -40 }, 1 } };
		const auto ortho = BuildLocalShadowPlan(requests, View, glm::ortho(-10.0f, 10.0f, -10.0f, 10.0f, -10.0f, 100.0f), 1);
		REQUIRE(ortho.Allocations.size() == 1);
		CHECK(ortho.Allocations[0].LightIndex == 1); // Same screen size independent of depth: UUID wins.
	}

	TEST_CASE("Coincident point and spot priority is stable and spot bounds follow the cone")
	{
		std::array requests = { LocalShadowRequest{ 42, 1, LocalShadowType::Spot, { 0, 0, -1 }, 10 },
								LocalShadowRequest{ 42, 0, LocalShadowType::Point, { 0, 0, -1 }, 10 } };
		for (int order = 0; order < 2; order++)
		{
			const auto plan = BuildLocalShadowPlan(requests, View, Projection, 6);
			REQUIRE(plan.Allocations.size() == 1);
			CHECK(plan.Allocations[0].LightIndex == 0);
			CHECK(plan.Maps.size() == 6);
			CHECK(plan.Skipped == 1);
			std::reverse(requests.begin(), requests.end());
		}
		const std::array away = { LocalShadowRequest{ 1, 0, LocalShadowType::Spot, { 0, 0, 5 }, 10, { 0, 0, 1 }, 30 } };
		CHECK(BuildLocalShadowPlan(away, View, Projection).Maps.empty());
		const std::array wide = { LocalShadowRequest{ 1, 0, LocalShadowType::Spot, { 0, 0, -5 }, 10, { 0, 0, 1 }, 80 } };
		CHECK(BuildLocalShadowPlan(wide, View, Projection).Maps.size() == 1);
	}

	TEST_CASE("Direct C++ softness has a finite bounded fallback")
	{
		CHECK(SanitizeLocalShadowSoftness(-1) == 0);
		CHECK(SanitizeLocalShadowSoftness(5) == MaxLocalShadowSoftness);
		CHECK(SanitizeLocalShadowSoftness(2) == 2);
		CHECK(SanitizeLocalShadowSoftness(std::numeric_limits<float>::quiet_NaN()) == 1);
		CHECK(SanitizeLocalShadowSoftness(std::numeric_limits<float>::infinity()) == 1);
		CHECK_FALSE(ValidateLocalShadowSoftness(std::numeric_limits<float>::quiet_NaN()).empty());
	}

	TEST_CASE("Scene shadow bias settings reject excessive and negative offsets")
	{
		for (const auto* field : { "ShadowBias", "ShadowNormalBias" })
		{
			const float maxValue = std::string_view(field) == "ShadowBias" ? 0.05f : 1.0f;
			for (float value : { -0.1f, maxValue + 0.001f })
			{
				Scene scene;
				std::string error;
				auto data = SceneSerializer::SerializeScene(scene);
				data["Renderer"][field] = value;
				CHECK_FALSE(SceneSerializer::DeserializeScene(scene, data, error));
				CHECK(error.find(field) != std::string::npos);
			}
		}
	}

	TEST_CASE("All six cube faces project their axes and overlap at seams with room for PCF")
	{
		const std::array requests = { LocalShadowRequest{ 1, 0, LocalShadowType::Point, { 1, 2, -3 }, 10 } };
		const auto plan = BuildLocalShadowPlan(requests, View, Projection);
		REQUIRE(plan.Maps.size() == 6);
		const std::array axes = { glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1) };
		for (size_t face = 0; face < 6; face++)
		{
			const auto& matrix = plan.Maps[face].ViewProjection;
			const auto clip = matrix * glm::vec4(requests[0].Position + axes[face] * 5.0f, 1.0f);
			CHECK(clip.x / clip.w == doctest::Approx(0).epsilon(1e-5));
			CHECK(clip.y / clip.w == doctest::Approx(0).epsilon(1e-5));
			CHECK(clip.z / clip.w > 0.0f);
			CHECK(clip.z / clip.w < 1.0f);
			const glm::vec3 other = axes[face].x == 0 ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
			const auto seam = matrix * glm::vec4(requests[0].Position + (axes[face] + other) * 3.0f, 1.0f);
			const float edge = std::max(std::abs(seam.x / seam.w), std::abs(seam.y / seam.w));
			CHECK((1.0f - edge) * LocalShadowTileSize * 0.5f >= MaxLocalShadowSoftness + 0.5f);
			const auto behind = matrix * glm::vec4(requests[0].Position - axes[face], 1.0f);
			CHECK(behind.w < 0.0f);
		}
	}

	TEST_CASE("Spot projections cover cone edges, vertical directions and very small ranges")
	{
		std::array requests = { LocalShadowRequest{ 1, 0, LocalShadowType::Spot, { 0, 0, -3 }, 10, { 0, 0, -1 }, 30 } };
		auto plan = BuildLocalShadowPlan(requests, View, Projection);
		REQUIRE(plan.Maps.size() == 1);
		auto clip = plan.Maps[0].ViewProjection * glm::vec4(5.0f * std::tan(glm::radians(30.0f)), 0, -8, 1);
		CHECK(clip.x / clip.w < 1.0f);
		CHECK(clip.x / clip.w > 0.95f);
		requests[0].Direction = { 0, 1, 0 };
		requests[0].Range = 0.001f;
		plan = BuildLocalShadowPlan(requests, View, Projection);
		REQUIRE(plan.Maps.size() == 1);
		clip = plan.Maps[0].ViewProjection * glm::vec4(0, 0.0005f, -3, 1);
		CHECK(std::isfinite(clip.z / clip.w));
		CHECK(clip.z / clip.w > 0.0f);
		CHECK(clip.z / clip.w < 1.0f);
	}

	TEST_CASE("Light shadow fields default safely, round trip, copy and reject invalid edits atomically")
	{
		auto scene = CreateRef<Scene>();
		Entity entity = scene->CreateEntityWithUUID(42, "Lights");
		for (const auto* name : { "PointLight", "SpotLight" })
		{
			std::string error;
			REQUIRE(ComponentRegistry::AddOrPatch(entity, name, { { "Intensity", 5 } }, error));
			const auto* info = ComponentRegistry::Find(name);
			REQUIRE(info);
			CHECK(info->Serialize(entity)["CastShadows"] == false);
			CHECK(info->Serialize(entity)["ShadowSoftness"] == 1.0f);
			REQUIRE(ComponentRegistry::AddOrPatch(entity, name, { { "ShadowSoftness", 0.0f } }, error));
			CHECK(info->Serialize(entity)["ShadowSoftness"] == 0.0f);
			REQUIRE(ComponentRegistry::AddOrPatch(entity, name, { { "CastShadows", true }, { "ShadowSoftness", 4.0f } }, error));
			const auto expected = info->Serialize(entity);
			for (const auto& bad : { nlohmann::json(-0.1f), nlohmann::json(4.1f), nlohmann::json("soft"), nlohmann::json(std::numeric_limits<double>::infinity()) })
			{
				CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, name, { { "CastShadows", false }, { "ShadowSoftness", bad } }, error));
				CHECK(error.find("ShadowSoftness") != std::string::npos);
				CHECK(info->Serialize(entity) == expected);
			}
			const char* range = std::string_view(name) == "PointLight" ? "Radius" : "Range";
			for (const auto& bad : { nlohmann::json(-1), nlohmann::json(std::numeric_limits<double>::infinity()) })
			{
				CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, name, { { range, bad } }, error));
				CHECK(error.find(range) != std::string::npos);
				CHECK(info->Serialize(entity) == expected);
			}
			CHECK_FALSE(ComponentRegistry::AddOrPatch(entity, name, { { "CastShadows", 1 } }, error));
			CHECK(info->Serialize(entity) == expected);
			const auto copy = Scene::Copy(scene);
			CHECK(info->Serialize(copy->GetEntityByUUID(42)) == expected);
			Entity restored = scene->CreateEntity();
			REQUIRE(info->Deserialize(restored, expected, error));
			CHECK(info->Serialize(restored) == expected);
		}
	}
}
