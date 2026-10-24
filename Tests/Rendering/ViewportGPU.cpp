#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "GPUFixture.h"

#include <Basalt/Animation/AnimationSystem.h>
#include <Basalt/Asset/AssetManager.h>
#include <Basalt/Asset/ImageUtils.h>
#include <Basalt/Asset/MeshOptimizer.h>
#include <Basalt/Core/FileSystem.h>

#include "../Source/TestUtils.h"
#include "../Source/MeshTestUtils.h"
#include <Basalt/Core/JobSystem.h>
#include <Basalt/Core/MemoryTracker.h>
#include <Basalt/Particles/ParticleSystem.h>
#include <Basalt/Renderer/SceneRenderer.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <latch>
#include <filesystem>

using namespace Basalt;

namespace {
	std::vector<uint8_t> Capture(SceneRenderer& renderer, Scene& scene, const RenderCamera& camera)
	{
		renderer.Render(scene, camera, 0.0f);
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
		renderer.SetSelections({});
		const auto plain = Capture(renderer, scene, camera);
		CHECK(renderer.GetStatistics().Triangles == 24);
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
		renderer.SetSelections({ front.GetUUID() });
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
		renderer.SetSelections({});
		CHECK(Capture(renderer, scene, camera) == plain);
	}
	// Dynamic render area shrinks inside the same allocation; output coordinates must still agree.
	renderer.SetRenderScale(1.0f);
	renderer.SetDynamicResolution({ true, UnreachableTargetFrameRate, 0.5f });
	renderer.SetViewportSize(101, 99);
	auto* depth = renderer.GetDepthTexture();
	for (int frame = 0; frame < 40; frame++)
	{
		renderer.Render(scene, camera, 0.0f);
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
	capture.Render(scene, camera, 0.0f);
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
	renderer.Render(scene, camera, 0.0f);
	CHECK(renderer.GetStatistics().Triangles == 24);
	ExpectPick(renderer, 50, 37, front.GetUUID());
	SceneRenderer reference(graphics->GetDevice());
	REQUIRE(reference.Initialize());
	reference.SetViewportSize(101, 99);
	CHECK(Capture(renderer, scene, camera) == Capture(reference, scene, camera));
	front.GetComponent<MaterialComponent>().AlbedoColor.a = 0.05f;
	renderer.Render(scene, camera, 0.0f);
	ExpectPick(renderer, 50, 37, back.GetUUID());
	front.RemoveComponent<MaterialComponent>();
	// Masked foreground fragments expose a different opaque background ID.
	back.AddComponent<MaterialComponent>();
	auto mesh = AssetManager::GetMesh("builtin://Cube");
	const auto savedMaterial = mesh->Materials[0];
	mesh->Materials[0].Alpha = AlphaMode::Mask;
	mesh->Materials[0].AlbedoColor.a = 0.0f;
	renderer.Render(scene, camera, 0.0f);
	ExpectPick(renderer, 50, 37, back.GetUUID());
	mesh->Materials[0] = savedMaterial;
	// A selected surface covering the viewport must not outline the image boundary.
	front.GetTransform().Scale = { 10, 10, 1 };
	renderer.SetSelections({});
	const auto clipped = Capture(renderer, scene, camera);
	renderer.SetSelections({ front.GetUUID() });
	CHECK(Capture(renderer, scene, camera) == clipped);
	// A fully occluded selection has no silhouette in the visible-surface ID target.
	front.GetTransform().Scale = glm::vec3(4);
	renderer.SetSelections({});
	const auto occluded = Capture(renderer, scene, camera);
	renderer.SetSelections({ back.GetUUID() });
	CHECK(Capture(renderer, scene, camera) == occluded);
	renderer.SetSelections({});
	front.GetTransform().Scale = glm::vec3(1);
	front.GetTransform().Translation.x = 10;
	renderer.Render(scene, camera, 0.0f);
	ExpectPick(renderer, 50, 37, back.GetUUID());
	scene.DestroyEntity(back);
	renderer.Render(scene, camera, 0.0f);
	ExpectPick(renderer, 50, 37, UUID(0));
	renderer.InvalidatePicking();
	CHECK_FALSE(renderer.Pick(50, 37));
	renderer.SetViewportSize(1, 1);
	renderer.Render(scene, camera, 0.0f);
	ExpectPick(renderer, 0, 0, UUID(0));
	SceneRenderer runtime(graphics->GetDevice());
	REQUIRE(runtime.Initialize());
	runtime.SetViewportSize(16, 16);
	runtime.Render(scene, camera, 0.0f);
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
		renderer.Render(scene, camera, 0.0f);
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

TEST_CASE("Viewport multi-selection outlines both surfaces and retains full UUID picking")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	scene.GetRendererSettings().SSAOEnabled = false;
	scene.GetRendererSettings().ShadowsEnabled = false;
	auto a = scene.CreateEntityWithUUID(UUID(0xf123456789abcdefULL), "Left");
	auto b = scene.CreateEntityWithUUID(UUID(0xabcdef9876543210ULL), "Right");
	for (Entity entity : { a, b })
		entity.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	a.GetTransform().Translation.x = -1;
	b.GetTransform().Translation.x = 1;
	RenderCamera camera;
	camera.Position = { 0, 0, 5 };
	camera.View = glm::lookAtRH(camera.Position, glm::vec3(0), glm::vec3(0, 1, 0));
	camera.Projection = glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, .1f, 20.0f);
	camera.Far = 20;
	SceneRenderer renderer(graphics->GetDevice(), { .Picking = true });
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(100, 100);
	for (float scale : { 1.0f, .5f, 1.5f })
	{
		renderer.SetRenderScale(scale);
		renderer.SetSelections({});
		const auto plain = Capture(renderer, scene, camera);
		renderer.SetSelections({ a.GetUUID(), b.GetUUID(), a.GetUUID() });
		const auto selected = Capture(renderer, scene, camera);
		ExpectPick(renderer, 25, 50, a.GetUUID());
		ExpectPick(renderer, 75, 50, b.GetUUID());
		size_t left = 0, right = 0;
		for (size_t i = 0; i < selected.size(); i += 4)
			if (selected[i] != plain[i] || selected[i + 1] != plain[i + 1] || selected[i + 2] != plain[i + 2])
			{
				CHECK(selected[i] == 255);
				CHECK(selected[i + 1] >= 190);
				CHECK(selected[i + 2] <= 26);
				if ((i / 4) % 100 < 50)
					++left;
				else
					++right;
			}
		CHECK(left > 100);
		CHECK(right > 100);
		renderer.SetSelections({});
		CHECK(Capture(renderer, scene, camera) == plain);
	}
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Async viewport draws a placeholder with every worker occupied then uploads published assets")
{
	auto graphics = CreateTestGraphics();
	AssetManager::Clear();
	JobSystem& pool = JobSystem::Get();
	pool.WaitIdle();
	const auto workers = pool.GetWorkerCount();
	std::latch started(workers);
	std::latch release(1);
	// Release even when a REQUIRE aborts the test, so the pool can always drain at process exit.
	auto unblock = std::shared_ptr<std::latch>(&release, [](std::latch* latch) {
		latch->count_down();
		JobSystem::Get().WaitIdle();
	});
	for (uint32_t i = 0; i < workers; ++i)
		(void)pool.Submit([&] {
			started.count_down();
			release.wait();
		});
	started.wait();

	Scene scene;
	scene.GetRendererSettings().SSAOEnabled = false;
	scene.GetRendererSettings().ShadowsEnabled = false;
	auto entity = scene.CreateEntity("Streaming sphere");
	entity.AddComponent<MeshComponent>().Mesh = "builtin://Sphere";
	RenderCamera camera;
	camera.Position = { 0, 0, 5 };
	camera.View = glm::lookAtRH(camera.Position, glm::vec3(0), glm::vec3(0, 1, 0));
	camera.Projection = glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, .1f, 20.0f);
	camera.Far = 20;
	const auto memoryBefore = MemoryTracker::Get(MemoryCategory::GpuBuffers).Bytes;
	{
		SceneRenderer renderer(graphics->GetDevice(), { .Picking = true, .AsyncLoading = true });
		REQUIRE(renderer.Initialize());
		renderer.SetViewportSize(64, 64);
		Capture(renderer, scene, camera);
		CHECK(renderer.GetStatistics().DrawCalls > 0);
		ExpectPick(renderer, 32, 32, entity.GetUUID());
		ExpectPick(renderer, 39, 24, entity.GetUUID());
		CHECK_FALSE(AssetManager::FindLoadedMesh("builtin://Sphere"));
		CHECK(AssetManager::IsLoading("builtin://Sphere"));
		CHECK(AssetManager::GetLoadedKeys() == std::vector<std::string>{ "builtin://Sphere" });
		CHECK(MemoryTracker::Get(MemoryCategory::GpuBuffers).Bytes > memoryBefore);
		unblock.reset();
		AssetManager::WaitForLoads();
		CHECK(AssetManager::FindLoadedMesh("builtin://Sphere"));
		Capture(renderer, scene, camera);
		CHECK(renderer.GetStatistics().DrawCalls > 0);
		ExpectPick(renderer, 32, 32, entity.GetUUID());
		CHECK(renderer.Pick(39, 24).value_or(UUID(0)) == UUID(0));
	}
	CHECK(MemoryTracker::Get(MemoryCategory::GpuBuffers).Bytes == memoryBefore);
	graphics->WaitIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
	AssetManager::Clear();
}

TEST_CASE("Particle billboards add exactly two color triangles per drawn particle")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	scene.GetRendererSettings().SSAOEnabled = false;
	scene.GetRendererSettings().ShadowsEnabled = false;
	auto emitter = scene.CreateEntity("Particles");
	auto& component = emitter.AddComponent<ParticleEmitterComponent>();
	component.PlayOnStart = false;
	component.Rate = 0;
	component.SpeedMin = 0;
	component.SpeedMax = 0;
	scene.OnRuntimeStart();
	REQUIRE(scene.GetParticleSystem());
	REQUIRE(scene.GetParticleSystem()->Emit(emitter, 7) == 7);
	RenderCamera camera;
	camera.Position = { 0, 0, 5 };
	camera.View = glm::lookAtRH(camera.Position, glm::vec3(0), glm::vec3(0, 1, 0));
	camera.Projection = glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, .1f, 20.0f);
	camera.Far = 20;
	SceneRenderer renderer(graphics->GetDevice());
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(64, 64);
	renderer.Render(scene, camera, 0);
	REQUIRE_FALSE(renderer.GetStatistics().Failed);
	CHECK(renderer.GetStatistics().Particles == 7);
	CHECK(renderer.GetStatistics().Triangles == 14);
	scene.OnRuntimeStop();
	graphics->WaitIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Opaque instance batches retain material constants, IDs and filtering through buffer growth")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	scene.GetRendererSettings().SSAOEnabled = false;
	auto sun = scene.CreateEntity("Sun");
	sun.AddComponent<DirectionalLightComponent>();
	sun.GetTransform().SetRotationEuler(glm::radians(glm::vec3(-50, 20, 0)));
	std::vector<Entity> cubes;
	constexpr uint32_t Count = 320;
	for (uint32_t i = 0; i < Count; ++i)
	{
		auto cube = scene.CreateEntity("Instance");
		cube.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
		cube.GetTransform().Translation = { static_cast<float>(i % 20) - 9.5f, static_cast<float>(i / 20) - 7.5f, 0 };
		cube.GetTransform().Scale = { i % 7 == 0 ? -.6f : .6f, .6f, .6f };
		cube.AddComponent<MaterialComponent>().AlbedoColor = { static_cast<float>(i % 6 + 1) / 8.0f, .2f, .8f, 1 };
		cubes.push_back(cube);
	}
	auto hidden = scene.CreateEntity("Off-camera instance");
	hidden.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	hidden.GetTransform().Translation.x = 1000;
	RenderCamera camera;
	camera.Position = { 0, 0, 5 };
	camera.View = glm::lookAtRH(camera.Position, glm::vec3(0), glm::vec3(0, 1, 0));
	camera.Projection = glm::orthoRH_ZO(-10.0f, 10.0f, -8.0f, 8.0f, .1f, 20.0f);
	camera.Far = 20;
	SceneRenderer renderer(graphics->GetDevice(), { .Picking = true });
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(400, 320);
	renderer.SetSelections({ cubes.back().GetUUID() });
	const auto batched = Capture(renderer, scene, camera);
	const auto stats = renderer.GetStatistics();
	CHECK(stats.DrawCalls == 1);
	CHECK(stats.Instances == Count);
	CHECK(stats.Triangles == Count * 12);
	CHECK(stats.ShadowDrawCalls > 0);
	for (uint32_t i : { 0u, 1u, 63u, 127u, 255u, Count - 1 })
		ExpectPick(renderer, 10 + i % 20 * 20, 310 - i / 20 * 20, cubes[i].GetUUID());
	renderer.SetInstancingEnabled(false);
	CHECK(Capture(renderer, scene, camera) == batched);
	CHECK(renderer.GetStatistics().DrawCalls == Count);
	CHECK(renderer.GetStatistics().Instances == stats.Instances);
	CHECK(renderer.GetStatistics().Triangles == stats.Triangles);
	CHECK(renderer.GetStatistics().ShadowDrawCalls > stats.ShadowDrawCalls);
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Batch key discovery keeps coplanar color and pick winners deterministic")
{
	auto graphics = CreateTestGraphics();
	SceneRenderer renderer(graphics->GetDevice(), { .Picking = true });
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(160, 160);
	RenderCamera camera;
	camera.Position = { 0, 0, 5 };
	camera.View = glm::lookAtRH(camera.Position, glm::vec3(0), glm::vec3(0, 1, 0));
	camera.Projection = glm::orthoRH_ZO(-1.5f, 1.5f, -1.5f, 1.5f, .1f, 20.0f);
	camera.Far = 20;
	// Reuse GPU resources but reverse discovery order: pointer order cannot satisfy both cases.
	for (bool cubeFirst : { true, false })
	{
		Scene scene;
		scene.GetRendererSettings().SSAOEnabled = false;
		scene.GetRendererSettings().ShadowsEnabled = false;
		for (uint32_t i = 0; i < 2; ++i)
		{
			const bool cube = (i == 0) == cubeFirst;
			auto entity = scene.CreateEntity("Coplanar");
			entity.AddComponent<MeshComponent>().Mesh = cube ? "builtin://Cube" : "builtin://Plane";
			entity.AddComponent<MaterialComponent>().AlbedoColor = cube ? glm::vec4(1, 0, 0, 1) : glm::vec4(0, 0, 1, 1);
			if (!cube)
			{
				entity.GetTransform().Translation.z = .5f;
				entity.GetTransform().SetRotationEuler({ glm::radians(90.0f), 0, 0 });
			}
		}
		renderer.SetInstancingEnabled(false);
		const auto reference = Capture(renderer, scene, camera);
		const auto picked = renderer.Pick(80, 80);
		REQUIRE(picked);
		renderer.SetInstancingEnabled(true);
		CHECK(Capture(renderer, scene, camera) == reference);
		CHECK(renderer.Pick(80, 80) == picked);
		CHECK(renderer.GetStatistics().DrawCalls == 2);
	}
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Shadow-only instances stay out of color and picking batches")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	scene.GetRendererSettings().SSAOEnabled = false;
	auto wall = scene.CreateEntity("Receiver");
	wall.AddComponent<MeshComponent>().Mesh = "builtin://Plane";
	wall.GetTransform().SetRotationEuler({ glm::radians(90.0f), 0, 0 });
	wall.GetTransform().Scale = { 6, 1, 3 };
	auto visible = scene.CreateEntity("Visible cube");
	visible.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	visible.GetTransform().Translation = { -1, 0, 1 };
	auto hidden = scene.CreateEntity("Off-camera caster");
	hidden.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	hidden.GetTransform().Translation = { 4, 0, 3 };
	auto sun = scene.CreateEntity("Sun");
	sun.AddComponent<DirectionalLightComponent>();
	sun.GetTransform().SetRotationEuler({ 0, glm::radians(33.7f), 0 });
	RenderCamera camera;
	camera.Position = { 0, 0, 5 };
	camera.View = glm::lookAtRH(camera.Position, glm::vec3(0), glm::vec3(0, 1, 0));
	camera.Projection = glm::orthoRH_ZO(-3.0f, 3.0f, -1.5f, 1.5f, .1f, 20.0f);
	camera.Far = 20;
	SceneRenderer renderer(graphics->GetDevice(), { .Picking = true });
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(300, 150);
	const auto withCaster = Capture(renderer, scene, camera);
	CHECK(renderer.GetStatistics().Instances == 2);
	ExpectPick(renderer, 100, 75, visible.GetUUID());
	renderer.SetInstancingEnabled(false);
	CHECK(Capture(renderer, scene, camera) == withCaster);
	CHECK(renderer.GetStatistics().Instances == 2);
	renderer.SetInstancingEnabled(true);
	hidden.GetComponent<MeshComponent>().CastShadows = false;
	CHECK(Capture(renderer, scene, camera) != withCaster);
	CHECK(renderer.GetStatistics().Instances == 2);
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Interleaved coplanar material keys use documented batch-group order")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	scene.GetRendererSettings().SSAOEnabled = false;
	scene.GetRendererSettings().ShadowsEnabled = false;
	const auto texturePath = std::filesystem::path(BASALT_ANIMATION_DATA_DIR).parent_path() / "FeatureTest/Assets/Textures/Checker.png";
	REQUIRE(AssetManager::GetTexture(texturePath.string()));
	Entity middle;
	for (uint32_t i = 0; i < 3; ++i)
	{
		auto entity = scene.CreateEntity("Coplanar cube");
		entity.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
		auto& material = entity.AddComponent<MaterialComponent>();
		material.AlbedoColor = { 1, 0, 0, 1 };
		if (i == 1)
		{
			middle = entity;
			material.AlbedoColor = { 0, 0, 1, 1 };
			material.AlbedoMap = texturePath.string();
		}
	}
	RenderCamera camera;
	camera.Position = { 0, 0, 5 };
	camera.View = glm::lookAtRH(camera.Position, glm::vec3(0), glm::vec3(0, 1, 0));
	camera.Projection = glm::orthoRH_ZO(-1.5f, 1.5f, -1.5f, 1.5f, .1f, 20.0f);
	camera.Far = 20;
	SceneRenderer renderer(graphics->GetDevice(), { .Picking = true });
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(160, 160);
	renderer.SetInstancingEnabled(false);
	const auto reference = Capture(renderer, scene, camera);
	const auto referenceDepth = renderer.PickSurface(80, 80);
	REQUIRE(referenceDepth);
	CHECK(renderer.Pick(80, 80) != middle.GetUUID());
	CHECK(renderer.GetStatistics().DrawCalls == 3);
	renderer.SetInstancingEnabled(true);
	CHECK(Capture(renderer, scene, camera) != reference);
	const auto groupedDepth = renderer.PickSurface(80, 80);
	REQUIRE(groupedDepth);
	// Identical geometry/transforms make this an actual depth tie, not a near-coplanar approximation.
	CHECK(groupedDepth->Depth == referenceDepth->Depth);
	ExpectPick(renderer, 80, 80, middle.GetUUID());
	CHECK(renderer.GetStatistics().DrawCalls == 2);
	CHECK(renderer.GetStatistics().Instances == 3);
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Soft lit particles sample copied depth across projection and target changes")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	scene.GetRendererSettings().SSAOEnabled = false;
	scene.GetRendererSettings().ShadowsEnabled = false;
	auto surface = scene.CreateEntity("Surface");
	surface.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	surface.GetTransform().Translation.z = -0.5f;
	auto emitter = scene.CreateEntity("Soft particles");
	emitter.GetTransform().Translation.z = 0.05f;
	auto& c = emitter.AddComponent<ParticleEmitterComponent>();
	c.PlayOnStart = false;
	c.Rate = 0;
	c.SpeedMin = c.SpeedMax = 0;
	c.StartSize = c.EndSize = 1;
	c.StartColor = c.EndColor = { 4, 0, 0, 1 };
	scene.OnRuntimeStart();
	SceneRenderer renderer(graphics->GetDevice());
	REQUIRE(renderer.Initialize());
	RenderCamera camera;
	camera.Position = { 0, 0, 5 };
	camera.View = glm::lookAtRH(camera.Position, glm::vec3(0), glm::vec3(0, 1, 0));
	camera.Far = 20;
	for (bool perspective : { false, true })
	{
		camera.Projection = perspective ? glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 0.1f, 20.0f) : glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 20.0f);
		for (float scale : { 1.0f, 0.5f, 1.5f })
		{
			renderer.SetRenderScale(scale);
			renderer.SetViewportSize(64, 64);
			scene.GetParticleSystem()->Stop(emitter, true);
			const auto baseline = Capture(renderer, scene, camera);
			REQUIRE(scene.GetParticleSystem()->Emit(emitter, 1) == 1);
			c.SoftDistance = 0;
			c.Lit = false;
			const auto hard = Capture(renderer, scene, camera);
			c.SoftDistance = 1;
			const auto soft = Capture(renderer, scene, camera);
			auto difference = [&](const auto& image) {
				uint64_t sum = 0;
				for (size_t i = 0; i < image.size(); ++i)
					sum += static_cast<uint64_t>(std::abs(int(image[i]) - int(baseline[i])));
				return sum;
			};
			CHECK(difference(hard) > 1000);
			CHECK(difference(soft) < difference(hard) / 2);
			c.SoftDistance = 0;
			c.Lit = true;
			CHECK(Capture(renderer, scene, camera) != hard);
		}
	}
	scene.OnRuntimeStop();
	graphics->WaitIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Lit particle diffuse uses bounded light attenuation and receives skylight irradiance")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	scene.GetRendererSettings().SSAOEnabled = false;
	scene.GetRendererSettings().ShadowsEnabled = false;
	scene.GetRendererSettings().AmbientColor = glm::vec3(0);
	scene.GetRendererSettings().Tonemap = Tonemapper::None;
	auto sky = scene.CreateEntity("Sky");
	auto& skyLight = sky.AddComponent<SkyLightComponent>();
	skyLight.Intensity = 0;
	skyLight.ShowBackground = false;
	auto light = scene.CreateEntity("Point");
	light.GetTransform().Translation.z = 0.1f;
	light.AddComponent<PointLightComponent>().Intensity = 0.1f;
	auto emitter = scene.CreateEntity("Diffuse particle");
	auto& c = emitter.AddComponent<ParticleEmitterComponent>();
	c.PlayOnStart = false;
	c.Rate = 0;
	c.SpeedMin = c.SpeedMax = 0;
	c.StartSize = c.EndSize = 2;
	c.StartColor = c.EndColor = glm::vec4(1);
	c.Lit = true;
	scene.OnRuntimeStart();
	REQUIRE(scene.GetParticleSystem()->Emit(emitter, 1) == 1);
	RenderCamera camera;
	camera.Position = { 0, 0, 5 };
	camera.View = glm::lookAtRH(camera.Position, glm::vec3(0), glm::vec3(0, 1, 0));
	camera.Projection = glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 20.0f);
	camera.Far = 20;
	SceneRenderer renderer(graphics->GetDevice());
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(64, 64);
	const auto lit = Capture(renderer, scene, camera);
	const size_t center = (32 * 64 + 32) * 4;
	// 0.1 radiance / PI with the shared near-unity attenuation is about 48 in display sRGB.
	CHECK(lit[center] > 25);
	CHECK(lit[center] < 75);
	light.GetComponent<PointLightComponent>().Intensity = 0;
	const auto dark = Capture(renderer, scene, camera);
	CHECK(dark[center] < 3);
	skyLight.Intensity = 1;
	const auto ambient = Capture(renderer, scene, camera);
	CHECK(ambient[center] > dark[center] + 10);
	scene.OnRuntimeStop();
	graphics->WaitIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Static LOD selection draws reduced ranges while picking retains entity identity")
{
	auto graphics = CreateTestGraphics();
	BasaltTest::TempDirectory directory("ImportedLODRender");
	AssetManager::TemporaryLoads temporary;
	const auto key = FileSystem::PathToGenericUtf8(BasaltTest::WriteSphereGltf(directory.GetPath()));
	auto source = AssetManager::GetMesh(key);
	REQUIRE_MESSAGE(source, AssetManager::GetError(key));
	REQUIRE_FALSE(source->Submeshes[0].LODs.empty());
	Scene scene;
	scene.GetRendererSettings().SSAOEnabled = false;
	scene.GetRendererSettings().ShadowsEnabled = false;
	auto entity = scene.CreateEntity("LOD");
	entity.AddComponent<MeshComponent>().Mesh = key;
	RenderCamera camera;
	camera.Position = { 0, 0, 5 };
	camera.View = glm::lookAtRH(camera.Position, glm::vec3(0), glm::vec3(0, 1, 0));
	camera.Projection = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, .1f, 20.0f);
	camera.Far = 20;
	SceneRenderer renderer(graphics->GetDevice(), { .Picking = true });
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(64, 64);
	Capture(renderer, scene, camera);
	const auto full = renderer.GetStatistics().Triangles;
	CHECK(full == source->Submeshes[0].IndexCount / 3);
	ExpectPick(renderer, 32, 32, entity.GetUUID());
	entity.GetComponent<MeshComponent>().LODDistance = 4;
	Capture(renderer, scene, camera);
	CHECK(renderer.GetStatistics().Triangles == source->Submeshes[0].LODs[0].IndexCount / 3);
	CHECK(renderer.GetStatistics().Triangles < full);
	ExpectPick(renderer, 32, 32, entity.GetUUID());
	auto nearEntity = scene.CreateEntity("NearLOD");
	nearEntity.AddComponent<MeshComponent>() = { key, 0, true, 4 };
	nearEntity.GetTransform().Translation = { .6f, 0, 2 };
	Capture(renderer, scene, camera);
	CHECK(renderer.GetStatistics().Triangles == full + source->Submeshes[0].LODs[0].IndexCount / 3);
	entity.AddComponent<MaterialComponent>().AlbedoColor.a = .5f;
	Capture(renderer, scene, camera);
	CHECK(renderer.GetStatistics().Triangles == 2 * full);
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Full thin transmission reveals an emissive destination instead of darkening the surface")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	scene.GetRendererSettings().SSAOEnabled = false;
	scene.GetRendererSettings().ShadowsEnabled = false;
	auto sky = scene.CreateEntity("DarkEnvironment");
	sky.AddComponent<SkyLightComponent>().Intensity = 0;
	auto back = scene.CreateEntity("EmissiveBackground");
	back.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	back.GetTransform().Scale = { 3, 3, 1 };
	auto& background = back.AddComponent<MaterialComponent>();
	background.AlbedoColor = { 0, 0, 0, 1 };
	background.EmissiveColor = { 0.2f, 0.4f, 0.6f };
	RenderCamera camera;
	camera.Position = { 0, 0, 5 };
	camera.View = glm::lookAtRH(camera.Position, glm::vec3(0), glm::vec3(0, 1, 0));
	camera.Projection = glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, .1f, 20.0f);
	camera.Far = 20;
	SceneRenderer renderer(graphics->GetDevice());
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(64, 64);
	const auto unobstructed = Capture(renderer, scene, camera);
	auto front = scene.CreateEntity("Glass");
	front.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	front.GetTransform().Translation = { 0, 0, 2 };
	auto& glass = front.AddComponent<MaterialComponent>();
	glass.Transmission = 1;
	glass.Metallic = 0;
	const auto through = Capture(renderer, scene, camera);
	const size_t centre = (size_t(32) * 64 + 32) * 4;
	for (size_t channel = 0; channel < 3; channel++)
		CHECK(std::abs(int(through[centre + channel]) - int(unobstructed[centre + channel])) <= 2);
	glass.Transmission = 0;
	const auto blocked = Capture(renderer, scene, camera);
	CHECK(std::abs(int(blocked[centre + 2]) - int(unobstructed[centre + 2])) > 40);

	// An opaque override (alpha 1) keeps full coverage with transmission: a white albedo map whose alpha is zero
	// must not punch holes, so half transmission still lets exactly half of the background through.
	BasaltTest::TempDirectory temp("TransmissionCoverage");
	std::string error;
	const std::vector<uint8_t> clear = { 255, 255, 255, 0 };
	REQUIRE(WritePng(temp.GetPath() / "clear.png", clear, 1, 1, error));
	glass.Transmission = 0.5f;
	const auto half = Capture(renderer, scene, camera);
	glass.AlbedoMap = FileSystem::PathToUtf8(temp.GetPath() / "clear.png");
	const auto halfWithMap = Capture(renderer, scene, camera);
	for (size_t channel = 0; channel < 3; channel++)
		CHECK(std::abs(int(halfWithMap[centre + channel]) - int(half[centre + channel])) <= 1);
	CHECK(std::abs(int(half[centre + 2]) - int(unobstructed[centre + 2])) > 20);
	// Translucent overrides still use albedo alpha as coverage.
	glass.AlbedoColor.a = 0.5f;
	CHECK(std::abs(int(Capture(renderer, scene, camera)[centre + 2]) - int(halfWithMap[centre + 2])) > 10);
	AssetManager::Clear();
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Clearcoat, transmission and UV-transformed maps render through shadows, SSAO and the prepass")
{
	auto graphics = CreateTestGraphics();
	BasaltTest::TempDirectory temp("ExtendedMaterialScene");
	std::string error;
	std::vector<uint8_t> checker(size_t{ 8 } * 8 * 4);
	for (size_t pixel = 0; pixel < 64; pixel++)
	{
		const uint8_t value = ((pixel % 8) / 2 + (pixel / 16)) % 2 ? 230 : 40;
		checker[pixel * 4] = value;
		checker[pixel * 4 + 1] = 128;
		checker[pixel * 4 + 2] = 220;
		checker[pixel * 4 + 3] = 255;
	}
	const auto map = FileSystem::PathToUtf8(temp.GetPath() / "checker.png");
	REQUIRE(WritePng(temp.GetPath() / "checker.png", checker, 8, 8, error));

	Scene scene;
	scene.GetRendererSettings().SSAOEnabled = true;
	scene.GetRendererSettings().ShadowsEnabled = true;
	auto sun = scene.CreateEntity("Sun");
	sun.AddComponent<DirectionalLightComponent>().CastShadows = true;
	sun.GetTransform().Rotation = glm::quat(glm::radians(glm::vec3(-50.0f, 30.0f, 0.0f)));
	auto ground = scene.CreateEntity("Ground");
	ground.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	ground.GetTransform().Translation = { 0, -1.5f, 0 };
	ground.GetTransform().Scale = { 6, 0.2f, 6 };
	RenderCamera camera;
	camera.Position = { 0, 1, 5 };
	camera.View = glm::lookAtRH(camera.Position, glm::vec3(0), glm::vec3(0, 1, 0));
	camera.Projection = glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 0.1f, 50.0f);
	camera.Far = 50;
	SceneRenderer renderer(graphics->GetDevice());
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(96, 96);
	const auto empty = Capture(renderer, scene, camera);

	const glm::vec3 positions[] = { { -1.2f, 0, 0 }, { 1.2f, 0, 0 } };
	for (size_t i = 0; i < 2; i++)
	{
		auto sphere = scene.CreateEntity("Sphere");
		sphere.AddComponent<MeshComponent>().Mesh = "builtin://Sphere";
		sphere.GetTransform().Translation = positions[i];
		auto& material = sphere.AddComponent<MaterialComponent>();
		material.AlbedoColor = { 0.8f, 0.3f, 0.2f, 1.0f };
		material.NormalMap = map;
		material.Clearcoat = 0.9f;
		material.ClearcoatRoughness = 0.1f;
		material.ClearcoatMap = material.ClearcoatRoughnessMap = material.ClearcoatNormalMap = map;
		material.UVScale = { 2, 3 };
		material.UVOffset = { 0.1f, 0.2f };
		material.UVRotation = 0.5f;
		// The right sphere also transmits, so the blended pass shades the same maps.
		if (i == 1)
		{
			material.Transmission = 0.6f;
			material.TransmissionMap = map;
		}
	}
	const auto shaded = Capture(renderer, scene, camera);
	REQUIRE(shaded.size() == empty.size());
	// The transmissive sphere's rim shows mostly background (within 20 of empty on NVIDIA), so sample its center.
	for (const uint32_t x : { 30u, 76u })
	{
		const size_t pixel = (size_t(48) * 96 + x) * 4;
		INFO("x " << x);
		CHECK(std::abs(int(shaded[pixel]) - int(empty[pixel])) + std::abs(int(shaded[pixel + 1]) - int(empty[pixel + 1])) + std::abs(int(shaded[pixel + 2]) - int(empty[pixel + 2])) > 20);
	}
	AssetManager::Clear();
	CHECK(graphics->GetValidationErrorCount() == 0);
}
