#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "GPUFixture.h"

#include <Basalt/Animation/AnimationSystem.h>
#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Renderer/SceneRenderer.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

using namespace Basalt;

namespace {
	std::vector<uint8_t> Capture(SceneRenderer& renderer, Scene& scene, const RenderCamera& camera)
	{
		renderer.Render(scene, camera);
		REQUIRE_FALSE(renderer.GetStatistics().Failed);
		std::vector<uint8_t> pixels;
		uint32_t width = 0, height = 0;
		REQUIRE(renderer.ReadOutputPixels(pixels, width, height));
		return pixels;
	}
	void ExpectPick(SceneRenderer& renderer, uint32_t x, uint32_t y, UUID expected)
	{
		const auto hit = renderer.Pick(x, y);
		REQUIRE(hit.has_value());
		if (!hit)
			return;
		CHECK(static_cast<uint64_t>(*hit) == static_cast<uint64_t>(expected));
	}
}

TEST_CASE("Viewport ID prepass selects visible surfaces across scaling, transparency and resource reuse")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	scene.GetRendererSettings().SSAOEnabled = false;
	scene.GetRendererSettings().ShadowsEnabled = false;
	auto back = scene.CreateEntityWithUUID(UUID(0xf123456789abcdefULL), "Back");
	back.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	back.GetTransform().Scale = glm::vec3(2);
	auto front = scene.CreateEntityWithUUID(UUID(0xabcdef9876543210ULL), "Front");
	front.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	front.GetTransform().Translation = { 0, 0.5f, 2 };
	RenderCamera camera;
	camera.Position = { 0, 0, 5 };
	camera.View = glm::lookAtRH(camera.Position, glm::vec3(0), glm::vec3(0, 1, 0));
	camera.Projection = glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, .1f, 20.0f);
	camera.Far = 20;
	SceneRenderer renderer(graphics->GetDevice(), { .GpuTimers = graphics->SupportsTimerQueries(), .Picking = true });
	REQUIRE(renderer.Initialize());
	CHECK_FALSE(renderer.Pick(0, 0));
	for (float scale : { 1.0f, 0.5f, 0.75f, 1.5f, 2.0f })
	{
		INFO(scale);
		renderer.SetRenderScale(scale);
		renderer.SetViewportSize(101, 99);
		CHECK_FALSE(renderer.Pick(0, 0));
		renderer.SetSelection(0);
		const auto plain = Capture(renderer, scene, camera);
		ExpectPick(renderer, 50, 37, front.GetUUID());
		const auto sample = renderer.PickSurface(50, 37);
		REQUIRE(sample);
		CHECK(sample->Depth == doctest::Approx((2.5f - .1f) / (20 - .1f)));
		CHECK(sample->UV.x >= 0);
		CHECK(sample->UV.x < 1);
		ExpectPick(renderer, 50, 65, back.GetUUID());
		ExpectPick(renderer, 5, 5, UUID(0));
		CHECK_FALSE(renderer.Pick(101, 0));
		CHECK_FALSE(renderer.Pick(0, 99));
		renderer.SetSelection(front.GetUUID());
		const auto outlined = Capture(renderer, scene, camera);
		size_t changes = 0;
		for (size_t i = 0; i < plain.size(); i += 4)
			if (plain[i] != outlined[i] || plain[i + 1] != outlined[i + 1] || plain[i + 2] != outlined[i + 2])
			{
				changes++;
				CHECK(outlined[i] == 255);
				CHECK(outlined[i + 1] >= 190);
				CHECK(outlined[i + 2] <= 26);
			}
		CHECK(changes > 100);
		CHECK(changes < 500);
		ExpectPick(renderer, 50, 37, front.GetUUID());
		renderer.SetSelection(0);
		CHECK(Capture(renderer, scene, camera) == plain);
	}
	// Dynamic render area shrinks inside the same allocation; output coordinates must still agree.
	renderer.SetRenderScale(1.0f);
	renderer.SetDynamicResolution({ true, UnreachableTargetFrameRate, 0.5f });
	renderer.SetViewportSize(101, 99);
	auto* depth = renderer.GetDepthTexture();
	for (int frame = 0; frame < 40; frame++)
	{
		renderer.Render(scene, camera);
		graphics->WaitIdle();
		CHECK(renderer.GetDepthTexture() == depth);
		ExpectPick(renderer, 50, 37, front.GetUUID());
		ExpectPick(renderer, 50, 65, back.GetUUID());
	}
	CHECK(renderer.GetRenderWidth() < renderer.GetViewportWidth());
	// A fresh capture at the effective scale must preserve the dynamically reduced picking grid.
	SceneRenderer capture(graphics->GetDevice(), { .Picking = true });
	REQUIRE(capture.Initialize());
	capture.SetRenderScale(renderer.GetCurrentRenderScale());
	capture.SetViewportSize(101, 99);
	capture.Render(scene, camera);
	CHECK(capture.GetRenderWidth() == renderer.GetRenderWidth());
	CHECK(capture.GetRenderHeight() == renderer.GetRenderHeight());
	for (uint32_t x = 30; x < 70; x++)
	{
		const auto shown = renderer.PickSurface(x, 37);
		const auto refreshed = capture.PickSurface(x, 37);
		REQUIRE(shown);
		REQUIRE(refreshed);
		CHECK(shown->EntityId == refreshed->EntityId);
		CHECK(shown->UV == refreshed->UV);
		CHECK(shown->Depth == doctest::Approx(refreshed->Depth));
	}
	renderer.SetDynamicResolution({});
	// Transparent nearest-surface picking must not replace the scene depth used by lighting.
	front.AddComponent<MaterialComponent>().AlbedoColor.a = 0.5f;
	renderer.Render(scene, camera);
	ExpectPick(renderer, 50, 37, front.GetUUID());
	SceneRenderer reference(graphics->GetDevice());
	REQUIRE(reference.Initialize());
	reference.SetViewportSize(101, 99);
	CHECK(Capture(renderer, scene, camera) == Capture(reference, scene, camera));
	front.GetComponent<MaterialComponent>().AlbedoColor.a = 0.05f;
	renderer.Render(scene, camera);
	ExpectPick(renderer, 50, 37, back.GetUUID());
	front.RemoveComponent<MaterialComponent>();
	// Masked foreground fragments expose a different opaque background ID.
	back.AddComponent<MaterialComponent>();
	auto mesh = AssetManager::GetMesh("builtin://Cube");
	const auto savedMaterial = mesh->Materials[0];
	mesh->Materials[0].Alpha = AlphaMode::Mask;
	mesh->Materials[0].AlbedoColor.a = 0.0f;
	renderer.Render(scene, camera);
	ExpectPick(renderer, 50, 37, back.GetUUID());
	mesh->Materials[0] = savedMaterial;
	// A selected surface covering the viewport must not outline the image boundary.
	front.GetTransform().Scale = { 10, 10, 1 };
	renderer.SetSelection(0);
	const auto clipped = Capture(renderer, scene, camera);
	renderer.SetSelection(front.GetUUID());
	CHECK(Capture(renderer, scene, camera) == clipped);
	// A fully occluded selection has no silhouette in the visible-surface ID target.
	front.GetTransform().Scale = glm::vec3(4);
	renderer.SetSelection(0);
	const auto occluded = Capture(renderer, scene, camera);
	renderer.SetSelection(back.GetUUID());
	CHECK(Capture(renderer, scene, camera) == occluded);
	renderer.SetSelection(0);
	front.GetTransform().Scale = glm::vec3(1);
	front.GetTransform().Translation.x = 10;
	renderer.Render(scene, camera);
	ExpectPick(renderer, 50, 37, back.GetUUID());
	scene.DestroyEntity(back);
	renderer.Render(scene, camera);
	ExpectPick(renderer, 50, 37, UUID(0));
	renderer.InvalidatePicking();
	CHECK_FALSE(renderer.Pick(50, 37));
	renderer.SetViewportSize(1, 1);
	renderer.Render(scene, camera);
	ExpectPick(renderer, 0, 0, UUID(0));
	SceneRenderer runtime(graphics->GetDevice());
	REQUIRE(runtime.Initialize());
	runtime.SetViewportSize(16, 16);
	runtime.Render(scene, camera);
	CHECK_FALSE(runtime.Pick(0, 0));
	CHECK(graphics->GetValidationErrorCount() == 0);
	AssetManager::Clear();
}

TEST_CASE("Viewport picking follows animated geometry and all mesh nodes map to the owning entity")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	scene.GetRendererSettings().SSAOEnabled = false;
	scene.GetRendererSettings().ShadowsEnabled = false;
	auto rig = scene.CreateEntityWithUUID(UUID(0xfedcba9876543210ULL), "Rig");
	rig.AddComponent<AnimatorComponent>().Mesh = std::string(BASALT_ANIMATION_DATA_DIR) + "/TwoJoint.gltf";
	RenderCamera camera;
	camera.Position = { 0, 1, 4 };
	camera.View = glm::lookAtRH(camera.Position, glm::vec3(0, 1, 0), glm::vec3(0, 1, 0));
	camera.Projection = glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, .1f, 10.0f);
	camera.Far = 10;
	SceneRenderer renderer(graphics->GetDevice(), { .Picking = true });
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(64, 64);
	auto mask = [&]() {
		renderer.Render(scene, camera);
		REQUIRE_FALSE(renderer.GetStatistics().Failed);
		std::vector<bool> result;
		for (uint32_t y = 2; y < 64; y += 4)
			for (uint32_t x = 2; x < 64; x += 4)
			{
				const auto hit = renderer.Pick(x, y);
				REQUIRE(hit);
				CHECK((*hit == UUID(0) || *hit == rig.GetUUID()));
				result.push_back(*hit == rig.GetUUID());
			}
		CHECK(std::count(result.begin(), result.end(), true) > 5);
		return result;
	};
	const auto rest = mask();
	scene.OnSimulationStart();
	REQUIRE(scene.GetAnimationSystem()->Play(rig, "Bend"));
	REQUIRE(scene.GetAnimationSystem()->Seek(rig, 0.75f));
	const auto posed = mask();
	CHECK(rest != posed);
	scene.OnSimulationStop();
	CHECK(mask() == rest);
	CHECK(graphics->GetValidationErrorCount() == 0);
	AssetManager::Clear();
}
