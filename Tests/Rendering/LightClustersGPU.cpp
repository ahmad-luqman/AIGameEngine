#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "GPUFixture.h"

#include <Basalt/Core/Log.h>
#include <Basalt/Renderer/GraphicsDevice.h>
#include <Basalt/Renderer/LightClusters.h>
#include <Basalt/Renderer/SceneRenderer.h>
#include <Basalt/Renderer/VulkanLoader.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <vector>

using namespace Basalt;

namespace {
	std::vector<uint32_t> ReadBuffer(nvrhi::IDevice* device, nvrhi::IBuffer* source)
	{
		nvrhi::BufferDesc desc;
		desc.byteSize = source->getDesc().byteSize;
		desc.cpuAccess = nvrhi::CpuAccessMode::Read;
		const auto staging = device->createBuffer(desc);
		REQUIRE(staging);
		const auto commands = device->createCommandList();
		REQUIRE(commands);
		commands->open();
		commands->copyBuffer(staging, 0, source, 0, desc.byteSize);
		commands->close();
		device->executeCommandList(commands);
		device->waitForIdle();
		const auto* data = device->mapBuffer(staging, nvrhi::CpuAccessMode::Read);
		REQUIRE(data);
		std::vector<uint32_t> result(desc.byteSize / sizeof(uint32_t));
		std::memcpy(result.data(), data, static_cast<size_t>(desc.byteSize));
		device->unmapBuffer(staging);
		return result;
	}
	std::vector<Entity> PopulateLightingScene(Scene& scene)
	{
		auto& settings = scene.GetRendererSettings();
		settings.SSAOEnabled = false;
		settings.AmbientColor = glm::vec3(0);
		for (int i = 0; i < 15; i++)
		{
			auto cube = scene.CreateEntity("Receiver");
			cube.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
			auto& material = cube.AddComponent<MaterialComponent>();
			material.AlbedoColor = { 0.6f, 0.4f, 0.2f, i % 3 == 0 ? 0.4f : 1.0f };
			const int row = i / 5;
			cube.GetComponent<TransformComponent>().Translation = { static_cast<float>(i % 5) - 2.0f, static_cast<float>(row) - 1.0f, -3.0f - static_cast<float>(i % 3) * 2.0f };
		}
		std::vector<Entity> localLights;
		for (uint32_t i = 0; i < MaxSceneLights - 1; i++)
		{
			auto light = scene.CreateEntity("Local light");
			const glm::vec3 position(static_cast<float>(i % 9) - 4.0f, static_cast<float>((i / 9) % 7) - 3.0f, -1.0f - static_cast<float>(i % 11));
			light.GetComponent<TransformComponent>().Translation = position;
			if (i % 2 == 0)
			{
				auto& point = light.AddComponent<PointLightComponent>();
				point.Radius = 1.8f;
				point.Intensity = 0.3f;
				point.CastShadows = i == 0;
			}
			else
			{
				auto& spot = light.AddComponent<SpotLightComponent>();
				spot.Range = 3.1f;
				spot.Intensity = 0.8f;
				spot.CastShadows = i == 1;
			}
			localLights.push_back(light);
		}
		for (int i = 0; i < 2; i++)
			scene.CreateEntity("Directional").AddComponent<DirectionalLightComponent>().Intensity = 0.1f;
		return localLights;
	}

	std::vector<uint8_t> CaptureScene(SceneRenderer& renderer, Scene& scene, const RenderCamera& camera, bool clustered, uint32_t expectedSkipped = 0)
	{
		renderer.SetClusteredLighting(clustered);
		REQUIRE(renderer.IsClusteredLighting() == clustered);
		renderer.Render(scene, camera, 0.0f);
		REQUIRE_FALSE(renderer.GetStatistics().Failed);
		REQUIRE(renderer.GetStatistics().DrawCalls > 0);
		REQUIRE(renderer.GetStatistics().LightsSkipped == expectedSkipped);
		std::vector<uint8_t> pixels;
		uint32_t width = 0, height = 0;
		REQUIRE(renderer.ReadOutputPixels(pixels, width, height));
		REQUIRE(pixels.size() == static_cast<size_t>(width) * height * 4);
		return pixels;
	}

}

TEST_CASE("GPU cluster lists match conservative CPU planes and preserve every index")
{
	auto graphics = CreateTestGraphics();
	nvrhi::IDevice* device = graphics->GetDevice();
	LightClusters clusters;
	REQUIRE(clusters.Initialize(device));
	nvrhi::IBuffer* buffer = clusters.GetBuffer();
	const auto commands = device->createCommandList();
	REQUIRE(commands);
	std::array<GpuLightData, MaxSceneLights> lights{};
	for (uint32_t i = 0; i < MaxSceneLights; i++)
	{
		lights[i].Position = { static_cast<float>(i % 9) - 4.0f, static_cast<float>((i / 9) % 7) - 3.0f, -0.3f - static_cast<float>(i) * 0.2f, 0.3f + static_cast<float>(i % 5) };
		lights[i].Color.w = static_cast<float>(i % 3); // Point, spot and unbounded directional.
	}
	for (bool orthographic : { false, true })
		for (int cameraMode : { 0, 1, 2 })
		{
			const auto depths = LightClusterDepths(orthographic ? -2.0f : 0.1f, 100.0f, orthographic);
			const auto projection = orthographic ? glm::ortho(-5.0f, 7.0f, -4.0f, 3.0f, depths.x, depths.y) : glm::frustum(-0.08f, 0.12f, -0.06f, 0.09f, depths.x, depths.y);
			auto view = cameraMode != 0 ? glm::lookAt(glm::vec3(3, 1, 5), glm::vec3(0, 0, -8), glm::vec3(0, 1, 0)) : glm::mat4(1);
			if (cameraMode == 2)
				view = glm::scale(view, glm::vec3(0.7f, 1.3f, 2.0f));
			commands->open();
			commands->writeBuffer(buffer, lights.data(), sizeof(lights));
			REQUIRE(clusters.Build(commands, view, projection, depths, MaxSceneLights));
			commands->close();
			device->executeCommandList(commands);
			const auto data = ReadBuffer(device, clusters.GetBuffer());
			const auto uploaded = std::bit_cast<std::array<uint32_t, LightClusterWordOffset>>(lights);
			CHECK(std::equal(uploaded.begin(), uploaded.end(), data.begin()));
			uint64_t total = 0;
			for (uint32_t z = 0; z < LightClusterDimensions.z; z++)
				for (uint32_t y = 0; y < LightClusterDimensions.y; y++)
					for (uint32_t x = 0; x < LightClusterDimensions.x; x++)
					{
						const uint32_t offset = LightClusterWordOffset + LightClusterListOffset(LightClusterIndex({ x, y, z }));
						const auto planes = LightClusterPlanes(view, projection, depths, { x, y, z });
						const uint32_t count = data[offset];
						REQUIRE(count <= MaxSceneLights);
						std::array<bool, MaxSceneLights> present{};
						for (uint32_t j = 0; j < count; j++)
						{
							const uint32_t i = data[offset + 1 + j];
							REQUIRE(i < MaxSceneLights);
							if (j > 0)
								REQUIRE(i > data[offset + j]);
							present[i] = true;
							const auto& light = lights[i];
							REQUIRE((light.Color.w == static_cast<float>(GpuLightType::Directional) || LightClusterIntersects(planes, glm::vec3(light.Position), light.Position.w, 2e-4f)));
						}
						// Require all zero-slack intersections; allow only a narrow rounding band of extras.
						for (uint32_t i = 0; i < MaxSceneLights; i++)
							if (lights[i].Color.w == static_cast<float>(GpuLightType::Directional) || LightClusterIntersects(planes, glm::vec3(lights[i].Position), lights[i].Position.w, 0))
								REQUIRE(present[i]);
						total += count;
					}
			CHECK(total < static_cast<uint64_t>(LightClusterCount) * MaxSceneLights / 2);
		}
	// Dense -> empty -> dense reuses UAVs: no stale counts, no per-cell overflow, last index retained.
	for (auto& light : lights)
		light.Position = { 0, 0, 0, 10000 };
	for (uint32_t count : { MaxSceneLights, 0u, 1u, MaxSceneLights, MaxSceneLights + 1 })
	{
		commands->open();
		commands->writeBuffer(buffer, lights.data(), sizeof(lights));
		REQUIRE(clusters.Build(commands, glm::mat4(1), glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f), LightClusterDepths(0.1f, 100.0f, false), count));
		commands->close();
		device->executeCommandList(commands);
		const auto data = ReadBuffer(device, clusters.GetBuffer());
		const auto uploaded = std::bit_cast<std::array<uint32_t, LightClusterWordOffset>>(lights);
		CHECK(std::equal(uploaded.begin(), uploaded.end(), data.begin()));
		for (uint32_t cell = 0; cell < LightClusterCount; cell++)
		{
			REQUIRE(data[LightClusterWordOffset + LightClusterListOffset(cell)] == std::min(count, MaxSceneLights));
			for (uint32_t i = 0; i < std::min(count, MaxSceneLights); i++)
				REQUIRE(data[LightClusterWordOffset + LightClusterListOffset(cell) + 1 + i] == i);
		}
	}
	// Tightness control: this sphere fits strictly inside exactly one x/y/depth cell.
	lights = {};
	lights[0].Position = { 0.5f, 0, -4.5f, 0.1f };
	commands->open();
	commands->writeBuffer(buffer, lights.data(), sizeof(lights));
	REQUIRE(clusters.Build(commands, glm::mat4(1), glm::ortho(-8.0f, 8.0f, -4.5f, 4.5f, 0.0f, 24.0f), LightClusterDepths(0, 24, true), 1));
	commands->close();
	device->executeCommandList(commands);
	const auto tight = ReadBuffer(device, buffer);
	for (uint32_t cell = 0; cell < LightClusterCount; cell++)
		REQUIRE(tight[LightClusterWordOffset + LightClusterListOffset(cell)] == (cell == LightClusterIndex({ 8, 4, 4 }) ? 1u : 0u));

	graphics->WaitIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Clustered scene images match the all-lights reference across camera and render-area changes")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	auto localLights = PopulateLightingScene(scene);
	SceneRendererSpecification rendererSpec;
	rendererSpec.GpuTimers = true;
	SceneRenderer renderer(graphics->GetDevice(), rendererSpec);
	REQUIRE(renderer.Initialize());
	RenderCamera camera;
	camera.Near = 0.1f;
	camera.Far = 30.0f;
	uint32_t expectedSkipped = 0;
	auto capture = [&](bool clustered) { return CaptureScene(renderer, scene, camera, clustered, expectedSkipped); };
	auto compare = [&]() { const auto reference = capture(false); CHECK((capture(true) == reference)); };
	for (bool orthographic : { false, true })
		for (const glm::uvec2 size : { glm::uvec2(97, 65), { 61, 83 } })
			for (float scale : { 1.0f, 0.5f })
			{
				camera.Near = orthographic ? -10.0f : 0.1f;
				camera.Projection = orthographic ? glm::ortho(-4.0f, 4.0f, -3.0f, 3.0f, camera.Near, camera.Far) : glm::perspective(glm::radians(70.0f), static_cast<float>(size.x) / static_cast<float>(size.y), camera.Near, camera.Far);
				camera.Position = orthographic ? glm::vec3(1, 0.2f, 1) : glm::vec3(0);
				camera.View = glm::lookAt(camera.Position, glm::vec3(0, 0, -5), glm::vec3(0, 1, 0));
				renderer.SetRenderScale(scale);
				renderer.SetViewportSize(size.x, size.y);
				const auto reference = capture(false);
				CHECK((capture(true) == reference));
			}
	// Run to the minimum scale, then compare both paths at the same area inside larger targets.
	renderer.SetViewportSize(96, 64);
	renderer.SetRenderScale(1.0f);
	renderer.SetDynamicResolution({ true, 1000000.0f, 0.5f });
	for (uint32_t frame = 0; frame < 40; frame++)
		capture(true);
	REQUIRE(renderer.IsDynamicResolutionActive());
	REQUIRE(renderer.GetRenderWidth() == 48);
	REQUIRE(renderer.GetRenderHeight() == 32);
	compare();
	renderer.SetDynamicResolution({});
	// Rebuild after lights cross both tile and depth boundaries, using the existing buffers.
	for (size_t i = 0; i < localLights.size(); i++)
		localLights[i].GetComponent<TransformComponent>().Translation += glm::vec3(i % 2 == 0 ? 1.9f : -0.7f, 0.4f, 1.3f);
	compare();

	// 255 locals plus one non-sun directional fill the scene buffer. Extra lights must be reported.
	for (uint32_t i = 0; i < 7; i++)
		localLights.push_back(scene.CreateEntity("Over budget"));
	for (size_t i = localLights.size() - 7; i < localLights.size(); i++)
		localLights[i].AddComponent<PointLightComponent>();
	expectedSkipped = 7;
	compare();

	const auto lit = capture(true);
	for (auto light : localLights)
		scene.DestroyEntity(light);
	expectedSkipped = 0;
	compare();
	CHECK((capture(true) != lit)); // Comparison cannot pass with local lighting disconnected.
	graphics->WaitIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Clustered lighting GPU timing benchmark")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	PopulateLightingScene(scene);
	SceneRendererSpecification spec;
	spec.GpuTimers = true;
	SceneRenderer renderer(graphics->GetDevice(), spec);
	REQUIRE(renderer.Initialize());
	RenderCamera camera;
	camera.Near = 0.1f;
	camera.Far = 30.0f;
	auto capture = [&](bool clustered) { return CaptureScene(renderer, scene, camera, clustered); };

	scene.GetRendererSettings().ShadowsEnabled = false;
	renderer.SetViewportSize(960, 540);
	camera.Projection = glm::perspective(glm::radians(70.0f), 960.0f / 540.0f, camera.Near, camera.Far);
	camera.Position = glm::vec3(0);
	camera.View = glm::mat4(1);
	for (bool clustered : { false, true })
	{
		for (uint32_t frame = 0; frame < GpuProfiler::AverageWindow + GpuProfiler::Latency + 2; frame++)
			capture(clustered);
		const auto& timings = renderer.GetGpuTimings();
		REQUIRE(timings.SampleCount > 0);
		float buildMs = -1, lightingMs = -1;
		for (const auto& pass : timings.Passes)
		{
			if (pass.Name == "Clusters")
				buildMs = pass.Milliseconds;
			if (pass.Name == "Lighting")
				lightingMs = pass.Milliseconds;
		}
		REQUIRE(lightingMs > 0);
		REQUIRE(buildMs >= 0);
		if (clustered)
			REQUIRE(buildMs > 0);
		BS_CORE_INFO("Cluster benchmark (960x540, 256 buffered lights): enabled={} build={:.3f} ms lighting={:.3f} ms", clustered, buildMs, lightingMs);
	}

	graphics->WaitIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Sparse lights exercise tile and slice lookup and fragment list consumption")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	auto& settings = scene.GetRendererSettings();
	settings.SSAOEnabled = false;
	settings.ShadowsEnabled = false;
	settings.AmbientColor = glm::vec3(0);
	auto& sky = scene.CreateEntity("No ambient").AddComponent<SkyLightComponent>();
	sky.Intensity = 0;
	sky.ShowBackground = false;
	RenderCamera camera;
	camera.Near = 0.1f;
	camera.Far = 40;
	camera.Projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, camera.Near, camera.Far);
	const auto depths = LightClusterDepths(camera.Near, camera.Far, false);
	std::vector<Entity> lights;
	for (uint32_t i = 0; i < 6; i++)
	{
		// Small influence spheres at different tile/depth boundaries reveal neighbouring-cell mistakes.
		const float depth = LightClusterBoundary(depths, 12 + i);
		const float u = (2.5f + 2.0f * static_cast<float>(i)) / 16.0f;
		const float v = (1.5f + 2.5f * static_cast<float>(i % 3)) / 9.0f;
		const glm::vec3 position((2 * u - 1) * depth / camera.Projection[0][0], (1 - 2 * v) * depth / camera.Projection[1][1], -depth);
		auto receiver = scene.CreateEntity("Sparse receiver");
		receiver.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
		receiver.AddComponent<MaterialComponent>().Roughness = 0.8f;
		receiver.GetComponent<TransformComponent>().Translation = position;
		receiver.GetComponent<TransformComponent>().Scale = { 0.6f, 0.6f, 0.02f };
		auto light = scene.CreateEntity("Sparse point");
		light.GetComponent<TransformComponent>().Translation = position + glm::vec3(0.04f, 0, 0.12f);
		auto& point = light.AddComponent<PointLightComponent>();
		point.Radius = 0.3f;
		point.Intensity = 0.5f;
		lights.push_back(light);
	}
	SceneRenderer renderer(graphics->GetDevice());
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(320, 180);
	const auto reference = CaptureScene(renderer, scene, camera, false);
	const auto clustered = CaptureScene(renderer, scene, camera, true);
	CHECK((clustered == reference));
	// Projection and metadata may differ in tools. Clamped edge cells must still include these lights.
	camera.Near = 3;
	camera.Far = 4;
	const auto mismatchedReference = CaptureScene(renderer, scene, camera, false);
	CHECK((CaptureScene(renderer, scene, camera, true) == mismatchedReference));
	camera.Near = 0.1f;
	camera.Far = 40;

	// Count visualization is driven by visits inside the PBR loop, not a CPU-side enable flag.
	// A disconnected setter or a shader forced to all-lights makes these images equal and fails.
	renderer.SetVisualizeLightCount(true);
	const auto allCounts = CaptureScene(renderer, scene, camera, false);
	const auto clusteredCounts = CaptureScene(renderer, scene, camera, true);
	auto sumColor = [](const std::vector<uint8_t>& pixels) {
		uint64_t sum = 0;
		for (size_t i = 0; i < pixels.size(); i += 4)
			sum += pixels[i] + pixels[i + 1] + pixels[i + 2];
		return sum;
	};
	CHECK(sumColor(clusteredCounts) > 0);
	CHECK(sumColor(clusteredCounts) < sumColor(allCounts));
	renderer.SetVisualizeLightCount(false);
	for (auto light : lights)
		scene.DestroyEntity(light);
	CHECK((CaptureScene(renderer, scene, camera, true) != clustered));
	graphics->WaitIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
}
