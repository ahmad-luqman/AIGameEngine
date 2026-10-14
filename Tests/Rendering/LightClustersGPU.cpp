#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

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
}

TEST_CASE("GPU cluster lists match conservative CPU planes and preserve every index")
{
	Log::Init();
	REQUIRE(VulkanLoader::Load());
	GraphicsDeviceSpecification spec;
	spec.EnableValidation = true;
	auto graphics = GraphicsDevice::CreateOffscreen(64, 64, spec);
	REQUIRE(graphics);
	REQUIRE(graphics->IsValidationActive());
	nvrhi::IDevice* device = graphics->GetDevice();
	LightClusters clusters;
	REQUIRE(clusters.Initialize(device));
	nvrhi::IBuffer* buffer = clusters.GetBuffer();
	const auto commands = device->createCommandList();
	REQUIRE(commands);
	std::array<GpuLightData, MaxClusterLights> lights{};
	for (uint32_t i = 0; i < MaxClusterLights; i++)
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
			clusters.Build(commands, view, projection, depths, MaxClusterLights);
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
						const uint32_t cell = (z * LightClusterDimensions.y + y) * LightClusterDimensions.x + x;
						const auto planes = LightClusterPlanes(view, projection, depths, { x, y, z });
						uint32_t expected = 0;
						for (uint32_t i = 0; i < MaxClusterLights; i++)
							if (lights[i].Color.w > 1.5f || LightClusterIntersects(planes, glm::vec3(lights[i].Position), lights[i].Position.w))
							{
								REQUIRE(expected < data[LightClusterWordOffset + cell * LightClusterStride]);
								REQUIRE(data[LightClusterWordOffset + cell * LightClusterStride + 1 + expected] == i);
								expected++;
							}
						REQUIRE(data[LightClusterWordOffset + cell * LightClusterStride] == expected);
						total += data[LightClusterWordOffset + cell * LightClusterStride];
					}
			CHECK(total < static_cast<uint64_t>(LightClusterCount) * MaxClusterLights / 2);
		}
	// Dense -> empty -> dense reuses UAVs: no stale counts, no per-cell overflow, last index retained.
	for (auto& light : lights)
		light.Position = { 0, 0, 0, 10000 };
	for (uint32_t count : { MaxClusterLights, 0u, 1u, MaxClusterLights })
	{
		commands->open();
		commands->writeBuffer(buffer, lights.data(), sizeof(lights));
		clusters.Build(commands, glm::mat4(1), glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f), LightClusterDepths(0.1f, 100.0f, false), count);
		commands->close();
		device->executeCommandList(commands);
		const auto data = ReadBuffer(device, clusters.GetBuffer());
		const auto uploaded = std::bit_cast<std::array<uint32_t, LightClusterWordOffset>>(lights);
		CHECK(std::equal(uploaded.begin(), uploaded.end(), data.begin()));
		for (uint32_t cell = 0; cell < LightClusterCount; cell++)
		{
			REQUIRE(data[LightClusterWordOffset + cell * LightClusterStride] == count);
			for (uint32_t i = 0; i < count; i++)
				REQUIRE(data[LightClusterWordOffset + cell * LightClusterStride + 1 + i] == i);
		}
	}
	graphics->WaitIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Clustered scene images match the all-lights reference across camera and render-area changes")
{
	Log::Init();
	REQUIRE(VulkanLoader::Load());
	GraphicsDeviceSpecification spec;
	spec.EnableValidation = true;
	auto graphics = GraphicsDevice::CreateOffscreen(64, 64, spec);
	REQUIRE(graphics);
	REQUIRE(graphics->IsValidationActive());
	Scene scene;
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
	for (uint32_t i = 0; i < MaxClusterLights - 1; i++)
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
	SceneRendererSpecification rendererSpec;
	rendererSpec.GpuTimers = true;
	SceneRenderer renderer(graphics->GetDevice(), rendererSpec);
	REQUIRE(renderer.Initialize());
	RenderCamera camera;
	camera.Near = 0.1f;
	camera.Far = 30.0f;
	auto capture = [&](bool clustered) {
		renderer.SetClusteredLighting(clustered);
		renderer.Render(scene, camera);
		REQUIRE_FALSE(renderer.GetStatistics().Failed);
		REQUIRE(renderer.GetStatistics().DrawCalls > 0);
		std::vector<uint8_t> pixels;
		uint32_t width = 0, height = 0;
		REQUIRE(renderer.ReadOutputPixels(pixels, width, height));
		REQUIRE(pixels.size() == static_cast<size_t>(width) * height * 4);
		return pixels;
	};
	for (bool orthographic : { false, true })
		for (const glm::uvec2 size : { glm::uvec2(97, 65), { 61, 83 } })
			for (float scale : { 1.0f, 0.5f })
			{
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
	CHECK((capture(true) == capture(false)));
	renderer.SetDynamicResolution({});
	// Rebuild after lights cross both tile and depth boundaries, using the existing buffers.
	for (size_t i = 0; i < localLights.size(); i++)
		localLights[i].GetComponent<TransformComponent>().Translation += glm::vec3(i % 2 == 0 ? 1.9f : -0.7f, 0.4f, 1.3f);
	CHECK((capture(true) == capture(false)));

	settings.ShadowsEnabled = false;
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
		float buildMs = 0, lightingMs = 0;
		for (const auto& pass : timings.Passes)
		{
			if (pass.Name == "Clusters")
				buildMs = pass.Milliseconds;
			if (pass.Name == "Lighting")
				lightingMs = pass.Milliseconds;
		}
		BS_CORE_INFO("Cluster benchmark (960x540, 256 buffered lights): enabled={} build={:.3f} ms lighting={:.3f} ms", clustered, buildMs, lightingMs);
	}

	const auto lit = capture(true);
	for (auto light : localLights)
		scene.DestroyEntity(light);
	CHECK((capture(true) == capture(false)));
	CHECK((capture(true) != lit)); // Comparison cannot pass with local lighting disconnected.
	graphics->WaitIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
}
