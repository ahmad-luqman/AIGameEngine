#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "GPUFixture.h"

#include <Basalt/Project/Project.h>
#include <Basalt/Renderer/PostProcessStack.h>
#include <Basalt/Renderer/RenderUtils.h>
#include <Basalt/Renderer/ShaderUtils.h>
#include <Basalt/Scene/Entity.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>

using namespace Basalt;

namespace {
	std::vector<uint8_t> Capture(SceneRenderer& renderer, Scene& scene, const RenderCamera& camera, float dt = 1.0f / 60.0f)
	{
		renderer.Render(scene, camera, dt);
		REQUIRE_FALSE(renderer.GetStatistics().Failed);
		std::vector<uint8_t> pixels;
		uint32_t width = 0, height = 0;
		REQUIRE(renderer.ReadOutputPixels(pixels, width, height));
		return pixels;
	}
	uint64_t Brightness(const std::vector<uint8_t>& pixels)
	{
		uint64_t sum = 0;
		for (size_t i = 0; i < pixels.size(); i += 4)
			sum += pixels[i] + pixels[i + 1] + pixels[i + 2];
		return sum;
	}
}

TEST_CASE("Post-process effects operate in order, reset exposure and bypass diagnostics")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	scene.CreateEntity("NoSky").AddComponent<SkyLightComponent>().ShowBackground = false;
	scene.GetRendererSettings().SSAOEnabled = false;
	scene.GetRendererSettings().AmbientColor = glm::vec3(0);
	Entity cube = scene.CreateEntity("Emissive");
	cube.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	auto& material = cube.AddComponent<MaterialComponent>();
	material.AlbedoColor = glm::vec4(0, 0, 0, 1);
	material.EmissiveColor = glm::vec3(1);
	material.EmissiveIntensity = 8;
	cube.GetTransform().Translation = { 0.1f, -0.1f, 0.5f };
	cube.GetTransform().Scale = glm::vec3(0.5f);
	cube.GetTransform().SetRotationEuler({ 0, 0, 0.4f });
	RenderCamera camera;
	SceneRenderer renderer(graphics->GetDevice());
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(64, 64);
	// Legacy settings keep their historical accepted ranges when no component overrides them.
	const auto fallback = scene.GetRendererSettings();
	scene.GetRendererSettings().Exposure = 0.00001f;
	scene.GetRendererSettings().SSAORadius = 101;
	Capture(renderer, scene, camera);
	scene.GetRendererSettings() = fallback;
	const auto legacy = Capture(renderer, scene, camera);
	std::filesystem::create_directories(BASALT_POSTPROCESS_OUTPUT_DIR);
	std::string imageError;
	REQUIRE(WritePng(std::filesystem::path(BASALT_POSTPROCESS_OUTPUT_DIR) / "Legacy.png", legacy, 64, 64, imageError));
	auto& post = scene.CreateEntity("Post").AddComponent<PostProcessComponent>();
	post.SSAOEnabled = false;
	CHECK(Capture(renderer, scene, camera) == legacy);
	// Authored exposure/tonemapper must win even when legacy values differ.
	scene.GetRendererSettings().Exposure = 4;
	scene.GetRendererSettings().Tonemap = Tonemapper::None;
	post.Enabled = false;
	const auto overrideReference = Capture(renderer, scene, camera);
	scene.GetRendererSettings().Exposure = 0.5f;
	scene.GetRendererSettings().Tonemap = Tonemapper::Reinhard;
	post.Enabled = true;
	post.Exposure = 4;
	post.Tonemap = Tonemapper::None;
	CHECK(Capture(renderer, scene, camera) == overrideReference);
	post.Exposure = std::numeric_limits<float>::quiet_NaN();
	renderer.Render(scene, camera, 0);
	CHECK(renderer.GetStatistics().Failed);
	post.Enabled = false;
	Capture(renderer, scene, camera);
	post = PostProcessComponent{};
	post.SSAOEnabled = false;
	scene.GetRendererSettings() = fallback;
	post.BloomEnabled = true;
	post.BloomIntensity = 0.5f;
	const auto bloom = Capture(renderer, scene, camera);
	REQUIRE(WritePng(std::filesystem::path(BASALT_POSTPROCESS_OUTPUT_DIR) / "Bloom.png", bloom, 64, 64, imageError));
	CHECK(Brightness(bloom) > Brightness(legacy));
	size_t glowPixels = 0;
	for (size_t i = 0; i < bloom.size(); i += 4)
		if (legacy[i] < 2 && bloom[i] > 5)
			++glowPixels;
	CHECK(glowPixels > 20);
	post.BloomThreshold = 100;
	CHECK(Capture(renderer, scene, camera) == legacy);
	post.BloomEnabled = false;
	post.FXAAEnabled = true;
	const auto fxaa = Capture(renderer, scene, camera);
	REQUIRE(WritePng(std::filesystem::path(BASALT_POSTPROCESS_OUTPUT_DIR) / "FXAA.png", fxaa, 64, 64, imageError));
	CHECK(fxaa != legacy);
	size_t edgePixels = 0;
	for (size_t i = 0; i < fxaa.size(); i += 4)
		if (fxaa[i] > 8 && fxaa[i] < 220)
			++edgePixels;
	CHECK(edgePixels > 10);
	post.FXAAEnabled = false;
	CHECK(Capture(renderer, scene, camera) == legacy);

	post.AutoExposure = true;
	material.EmissiveIntensity = 0.25f;
	const auto adaptedLow = Capture(renderer, scene, camera);
	material.EmissiveIntensity = 4;
	const auto unadaptedHigh = Capture(renderer, scene, camera, 0);
	CHECK(Brightness(unadaptedHigh) > Brightness(adaptedLow) * 2);
	const auto partAdapted = Capture(renderer, scene, camera, 0.1f);
	CHECK(Brightness(partAdapted) < Brightness(unadaptedHigh));
	std::vector<uint8_t> adaptedHigh;
	for (int i = 0; i < 30; ++i)
		adaptedHigh = Capture(renderer, scene, camera, 0.1f);
	CHECK(Brightness(adaptedHigh) < Brightness(partAdapted));
	CHECK(Brightness(adaptedHigh) > 0);
	// Explicit capture reset is independent of prior luminance and picking/capture history.
	renderer.ResetExposure();
	const auto reset = Capture(renderer, scene, camera, 0);
	SceneRenderer fresh(graphics->GetDevice());
	REQUIRE(fresh.Initialize());
	fresh.SetViewportSize(64, 64);
	CHECK(Capture(fresh, scene, camera, 0) == reset);
	post.AutoExposure = false;
	Capture(renderer, scene, camera);
	post.AutoExposure = true;
	CHECK(Capture(renderer, scene, camera, 0) == reset);
	// Selecting a different component resets adapted history without an explicit capture reset.
	auto contender = scene.CreateEntity("OtherPost");
	auto& other = contender.AddComponent<PostProcessComponent>();
	other = post;
	other.Enabled = false;
	material.EmissiveIntensity = 0.25f;
	for (int i = 0; i < 20; ++i)
		Capture(renderer, scene, camera, 0.1f);
	material.EmissiveIntensity = 4;
	const auto beforeSelection = Capture(renderer, scene, camera, 0);
	other.Enabled = true;
	other.Priority = post.Priority + 1;
	const auto selectionReset = Capture(renderer, scene, camera, 0);
	CHECK(selectionReset != beforeSelection);
	renderer.ResetExposure();
	CHECK(Capture(renderer, scene, camera, 0) == selectionReset);
	other.Enabled = false;
	post.MinExposure = post.MaxExposure = 1;
	const auto clamped = Capture(renderer, scene, camera);
	post.AutoExposure = false;
	CHECK(Capture(renderer, scene, camera) == clamped);
	post.AutoExposure = true;

	for (auto debug : { RendererDebugView::Depth, RendererDebugView::Normals, RendererDebugView::SSAO })
	{
		scene.GetRendererSettings().DebugView = debug;
		post.Enabled = false;
		const auto reference = Capture(renderer, scene, camera);
		post.Enabled = true;
		post.BloomEnabled = true;
		post.FXAAEnabled = true;
		CHECK(Capture(renderer, scene, camera) == reference);
	}
	graphics->WaitIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Post-processing survives resize and matches tight and dynamic allocations with FSR")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	scene.CreateEntity("NoSky").AddComponent<SkyLightComponent>().ShowBackground = false;
	scene.GetRendererSettings().AmbientColor = glm::vec3(0);
	auto cube = scene.CreateEntity("Edges");
	cube.AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	cube.AddComponent<MaterialComponent>().EmissiveColor = { 2, 0.5f, 0.1f };
	cube.GetTransform().Translation = { 0.1f, -0.1f, 0.5f };
	cube.GetTransform().Scale = glm::vec3(0.7f);
	auto& post = scene.CreateEntity("Post").AddComponent<PostProcessComponent>();
	post.BloomEnabled = true;
	post.FXAAEnabled = true;
	post.AutoExposure = true;
	post.SSAOEnabled = false;
	SceneRenderer renderer(graphics->GetDevice(), { .GpuTimers = true });
	REQUIRE(renderer.Initialize());
	RenderCamera camera;
	for (bool fsr : { false, true })
		for (glm::uvec2 size : { glm::uvec2(64, 64), { 83, 47 }, { 1, 7 }, { 7, 1 }, { 1, 1 } })
		{
			CAPTURE(fsr);
			CAPTURE(size.x);
			CAPTURE(size.y);
			scene.GetRendererSettings().FSR1Enabled = fsr;
			renderer.SetRenderScale(0.75f);
			renderer.SetDynamicResolution({ true, 1000000.0f, 0.25f });
			renderer.SetViewportSize(size.x, size.y);
			// Readback completes submissions so query feedback is available without wall-clock assumptions.
			for (int frame = 0; frame < 32 && renderer.GetCurrentRenderScale() > 0.3f; ++frame)
				Capture(renderer, scene, camera);
			REQUIRE(renderer.GetCurrentRenderScale() <= 0.3f);
			renderer.ResetExposure();
			const auto padded = Capture(renderer, scene, camera);
			SceneRenderer tight(graphics->GetDevice());
			REQUIRE(tight.Initialize());
			tight.SetRenderScale(renderer.GetCurrentRenderScale());
			tight.SetViewportSize(size.x, size.y);
			const auto expected = Capture(tight, scene, camera);
			CHECK(renderer.GetRenderWidth() == tight.GetRenderWidth());
			CHECK(renderer.GetRenderHeight() == tight.GetRenderHeight());
			// Half-float interpolation can differ by up to two final byte values for non-power-of-two UV scales.
			REQUIRE(padded.size() == expected.size());
			int largest = 0;
			for (size_t i = 0; i < padded.size(); ++i)
				largest = std::max(largest, std::abs(static_cast<int>(padded[i]) - expected[i]));
			CHECK(largest <= 2);
		}
	graphics->WaitIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Histogram and bloom ignore poisoned texels outside the active render area")
{
	auto graphics = CreateTestGraphics();
	auto* device = graphics->GetDevice();
	const auto fullscreen = CreateEmbeddedShader(device, "Fullscreen.vert", nvrhi::ShaderType::Vertex);
	const auto sampler = device->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
	REQUIRE(fullscreen);
	REQUIRE(sampler);
	PostProcessStack stack;
	const auto commands = device->createCommandList();
	REQUIRE(commands);
	auto render = [&](glm::uvec2 target, glm::uvec2 area, float poison, bool bloom, bool automatic, float input = 0.5f, float activePoison = 0.0f, bool poisonActive = false) {
		nvrhi::TextureDesc desc;
		desc.width = target.x;
		desc.height = target.y;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		const auto source = device->createTexture(desc);
		REQUIRE(source);
		REQUIRE(stack.Prepare(device, source, fullscreen, sampler));
		std::vector<uint16_t> pixels(static_cast<size_t>(target.x) * target.y * 4);
		for (uint32_t y = 0; y < target.y; ++y)
			for (uint32_t x = 0; x < target.x; ++x)
			{
				float value = x < area.x && y < area.y ? input * (x > y ? 4.0f : 1.0f) : poison;
				if (poisonActive && x == area.x / 2 && y == area.y / 2)
					value = activePoison;
				const size_t offset = (static_cast<size_t>(y) * target.x + x) * 4;
				for (size_t c = 0; c < 3; ++c)
					pixels[offset + c] = glm::packHalf1x16(value);
				pixels[offset + 3] = glm::packHalf1x16(1.0f);
			}
		PostProcessComponent settings;
		settings.BloomEnabled = bloom;
		settings.BloomThreshold = 0.1f;
		settings.BloomIntensity = 0.5f;
		settings.AutoExposure = automatic;
		commands->open();
		commands->writeTexture(source, 0, 0, pixels.data(), static_cast<size_t>(target.x) * 8);
		stack.RenderHDR(commands, area, settings, 0.1f, true);
		nvrhi::TextureDesc stagingDesc;
		stagingDesc.width = area.x;
		stagingDesc.height = area.y;
		stagingDesc.format = nvrhi::Format::RGBA16_FLOAT;
		const auto staging = device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
		REQUIRE(staging);
		nvrhi::TextureSlice slice;
		slice.width = area.x;
		slice.height = area.y;
		commands->copyTexture(staging, nvrhi::TextureSlice(), stack.GetHDR(), slice);
		commands->close();
		device->executeCommandList(commands);
		device->waitForIdle();
		size_t rowPitch = 0;
		const auto* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
		REQUIRE(mapped);
		std::vector<uint16_t> result(static_cast<size_t>(area.x) * area.y * 4);
		for (uint32_t y = 0; y < area.y; ++y)
			std::memcpy(result.data() + static_cast<size_t>(y) * area.x * 4, mapped + y * rowPitch, static_cast<size_t>(area.x) * 8);
		device->unmapStagingTexture(staging);
		return result;
	};
	for (const glm::uvec2 area : { glm::uvec2(1, 1), { 1, 7 }, { 7, 1 }, { 17, 13 }, { 32, 32 } })
		for (bool bloom : { false, true })
			for (bool automatic : { false, true })
			{
				CAPTURE(area.x);
				CAPTURE(area.y);
				CAPTURE(bloom);
				CAPTURE(automatic);
				const auto tight = render(area, area, 0, bloom, automatic);
				for (float poison : { 1000.0f, std::numeric_limits<float>::quiet_NaN() })
				{
					CAPTURE(poison);
					const auto padded = render({ 79, 73 }, area, poison, bloom, automatic);
					REQUIRE(tight.size() == padded.size());
					for (size_t i = 0; i < tight.size(); ++i)
					{
						CAPTURE(i);
						CHECK(glm::unpackHalf1x16(tight[i]) == doctest::Approx(glm::unpackHalf1x16(padded[i])).epsilon(0.003));
					}
				}
			}
	for (float invalid : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
	{
		const auto reference = render({ 17, 13 }, { 17, 13 }, 0, false, true, 0.5f, 0.0f, true);
		const auto metered = render({ 17, 13 }, { 17, 13 }, 0, false, true, 0.5f, invalid, true);
		for (size_t i = 0; i < reference.size(); i += 4)
			if (i / 4 != 6 * 17 + 8)
				CHECK(glm::unpackHalf1x16(reference[i]) == doctest::Approx(glm::unpackHalf1x16(metered[i])).epsilon(0.003));
		const auto result = render({ 17, 13 }, { 17, 13 }, 0, true, false, 0.5f, invalid, true);
		for (size_t i = 0; i < result.size(); i += 4)
		{
			if (i / 4 == 6 * 17 + 8)
				continue; // The original invalid HDR pixel itself is outside bloom's responsibility.
			CHECK(std::isfinite(glm::unpackHalf1x16(result[i])));
			CHECK(glm::unpackHalf1x16(result[i]) < 4.0f);
		}
	}
	const auto black = render({ 17, 13 }, { 7, 9 }, 1000, false, true, 0);
	for (size_t i = 0; i < black.size(); i += 4)
		CHECK(black[i] == 0);
	graphics->WaitIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
}

TEST_CASE("Session quality settings cap authored shadows and SSAO at render time")
{
	auto graphics = CreateTestGraphics();
	Scene scene;
	scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().ShowBackground = false;
	auto floor = scene.CreateEntity("Floor");
	floor.AddComponent<MeshComponent>().Mesh = "builtin://Plane";
	floor.GetTransform().Scale = glm::vec3(10);
	floor.GetTransform().Translation.y = -0.5f;
	scene.CreateEntity("Cube").AddComponent<MeshComponent>().Mesh = "builtin://Cube";
	auto sun = scene.CreateEntity("Sun");
	sun.AddComponent<DirectionalLightComponent>();
	sun.GetTransform().SetRotationEuler(glm::radians(glm::vec3(-50, 35, 0)));
	auto point = scene.CreateEntity("Point");
	point.AddComponent<PointLightComponent>().CastShadows = true;
	point.GetTransform().Translation = { 2, 3, 2 };
	auto& post = scene.CreateEntity("Post").AddComponent<PostProcessComponent>();
	post.SSAOEnabled = true;
	scene.OnRuntimeStart();
	RenderCamera camera;
	camera.Position = { 3, 3, 5 };
	camera.View = glm::lookAt(camera.Position, glm::vec3(0), glm::vec3(0, 1, 0));
	camera.Projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
	camera.Far = 100;
	SceneRenderer renderer(graphics->GetDevice());
	REQUIRE(renderer.Initialize());
	renderer.SetViewportSize(64, 64);
	const auto high = Capture(renderer, scene, camera);
	CHECK(renderer.GetStatistics().ShadowDrawCalls > 0);
	CHECK(renderer.GetStatistics().LocalShadowMaps == 6);
	std::string error;
	REQUIRE(scene.SetGraphicsSettings({ { "ShadowQuality", "Low" } }, error));
	const auto low = Capture(renderer, scene, camera);
	CHECK(low != high);
	CHECK(renderer.GetStatistics().LocalShadowMaps == 6);
	REQUIRE(scene.SetGraphicsSettings({ { "ShadowQuality", "Off" } }, error));
	const auto off = Capture(renderer, scene, camera);
	CHECK(off != high);
	CHECK(renderer.GetStatistics().ShadowDrawCalls == 0);
	CHECK(renderer.GetStatistics().LocalShadowMaps == 0);
	scene.GetRendererSettings().DebugView = RendererDebugView::SSAO;
	const auto aoHigh = Capture(renderer, scene, camera);
	REQUIRE(scene.SetGraphicsSettings({ { "SSAOQuality", "Low" } }, error));
	CHECK(Capture(renderer, scene, camera) != aoHigh);
	REQUIRE(scene.SetGraphicsSettings({ { "SSAOQuality", "Off" } }, error));
	const auto aoOff = Capture(renderer, scene, camera);
	for (size_t i = 0; i < aoOff.size(); i += 4)
		CHECK(aoOff[i] == 255);
	REQUIRE(scene.SetGraphicsSettings({ { "SSAOQuality", "High" } }, error));
	post.SSAOEnabled = false;
	CHECK(Capture(renderer, scene, camera) == aoOff);
	scene.OnRuntimeStop();
	// Own the old project while SetActive replaces the global reference.
	Ref<Project> previousProject = Project::GetActive(); // NOLINT(performance-unnecessary-copy-initialization): SetActive replaces the referenced global.
	auto project = CreateRef<Project>();
	project->GetConfig().Graphics.ShadowQuality = GraphicsQuality::Off;
	project->GetConfig().Graphics.SSAOQuality = GraphicsQuality::Off;
	Project::SetActive(project);
	post.SSAOEnabled = true;
	CHECK(Capture(renderer, scene, camera) == aoOff);
	scene.GetRendererSettings().DebugView = RendererDebugView::None;
	Capture(renderer, scene, camera);
	CHECK(renderer.GetStatistics().ShadowDrawCalls == 0);
	CHECK(renderer.GetStatistics().LocalShadowMaps == 0);
	Project::SetActive(previousProject);
	graphics->WaitIdle();
	CHECK(graphics->GetValidationErrorCount() == 0);
}
