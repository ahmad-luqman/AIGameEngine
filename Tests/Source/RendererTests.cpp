#include <doctest/doctest.h>

#include <Basalt/Renderer/DeviceSelection.h>
#include <Basalt/Renderer/DynamicResolution.h>
#include <Basalt/Renderer/GpuProfiler.h>
#include <Basalt/Renderer/RenderUtils.h>
#include <Basalt/Renderer/SceneLights.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

#include <glm/gtc/epsilon.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <deque>
#include <limits>
#include <vector>

using namespace Basalt;

TEST_SUITE("DeviceSelection")
{
	// A laptop with hybrid graphics plus Mesa's software rasterizer, in a typical enumeration order.
	const std::vector<PhysicalDeviceInfo> HybridLaptop = {
		{ "Intel(R) UHD Graphics 630", PhysicalDeviceKind::Integrated, true },
		{ "NVIDIA GeForce RTX 3060 Laptop GPU", PhysicalDeviceKind::Discrete, true },
		{ "llvmpipe (LLVM 17.0.6, 256 bits)", PhysicalDeviceKind::Cpu, true },
	};

	TEST_CASE("Automatic selection prefers discrete, then integrated, then CPU")
	{
		std::string error;
		CHECK(SelectPhysicalDevice(HybridLaptop, "", error) == 1u);

		const std::vector<PhysicalDeviceInfo> noDiscrete = { HybridLaptop[2], HybridLaptop[0] };
		CHECK(SelectPhysicalDevice(noDiscrete, "", error) == 1u);

		// lavapipe alone (CI): a CPU device is still usable.
		const std::vector<PhysicalDeviceInfo> cpuOnly = { HybridLaptop[2] };
		CHECK(SelectPhysicalDevice(cpuOnly, "", error) == 0u);
	}

	TEST_CASE("Automatic selection skips unsuitable devices and keeps enumeration order on ties")
	{
		std::vector<PhysicalDeviceInfo> devices = HybridLaptop;
		devices[1].Suitable = false;
		std::string error;
		CHECK(SelectPhysicalDevice(devices, "", error) == 0u);

		const std::vector<PhysicalDeviceInfo> twoDiscrete = {
			{ "GPU A", PhysicalDeviceKind::Discrete, true },
			{ "GPU B", PhysicalDeviceKind::Discrete, true },
		};
		CHECK(SelectPhysicalDevice(twoDiscrete, "", error) == 0u);
	}

	TEST_CASE("Automatic selection fails with a message when nothing is suitable")
	{
		std::vector<PhysicalDeviceInfo> devices = HybridLaptop;
		for (PhysicalDeviceInfo& device : devices)
			device.Suitable = false;
		std::string error;
		CHECK_FALSE(SelectPhysicalDevice(devices, "", error).has_value());
		CHECK_FALSE(error.empty());
		CHECK_FALSE(SelectPhysicalDevice({}, "", error).has_value());
	}

	TEST_CASE("An index selects the device at that enumeration position")
	{
		std::string error;
		CHECK(SelectPhysicalDevice(HybridLaptop, "0", error) == 0u);
		CHECK(SelectPhysicalDevice(HybridLaptop, "2", error) == 2u);
		CHECK_FALSE(SelectPhysicalDevice(HybridLaptop, "3", error).has_value());
		CHECK(error.find("available") != std::string::npos);
		CHECK_FALSE(SelectPhysicalDevice(HybridLaptop, "99999999999999999999999", error).has_value());
	}

	TEST_CASE("A name selector is a case-insensitive substring that must match exactly one device")
	{
		std::string error;
		CHECK(SelectPhysicalDevice(HybridLaptop, "nvidia", error) == 1u);
		CHECK(SelectPhysicalDevice(HybridLaptop, "INTEL", error) == 0u);
		CHECK(SelectPhysicalDevice(HybridLaptop, "llvmpipe", error) == 2u);

		CHECK_FALSE(SelectPhysicalDevice(HybridLaptop, "radeon", error).has_value());
		CHECK(error.find("matches no GPU") != std::string::npos);

		// "g" appears in two names ("Graphics", "GeForce"): ambiguous, never "first match".
		CHECK_FALSE(SelectPhysicalDevice(HybridLaptop, "g", error).has_value());
		CHECK(error.find("ambiguous") != std::string::npos);
	}

	TEST_CASE("An explicit selector never falls back from an unsuitable device")
	{
		std::vector<PhysicalDeviceInfo> devices = HybridLaptop;
		devices[0].Suitable = false;
		std::string error;
		CHECK_FALSE(SelectPhysicalDevice(devices, "intel", error).has_value());
		CHECK(error.find("Intel") != std::string::npos);
		CHECK_FALSE(SelectPhysicalDevice(devices, "0", error).has_value());
	}
}

TEST_SUITE("SceneLights")
{
	Entity AddDirectionalLight(Scene & scene, UUID id, float intensity, bool castShadows)
	{
		Entity entity = scene.CreateEntityWithUUID(id, "Light");
		auto& light = entity.AddComponent<DirectionalLightComponent>();
		light.Intensity = intensity;
		light.CastShadows = castShadows;
		return entity;
	}

	std::vector<uint64_t> CollectIds(Scene & scene)
	{
		std::vector<uint64_t> ids;
		for (const DirectionalLightInfo& light : CollectDirectionalLights(scene))
			ids.push_back(static_cast<uint64_t>(light.ID));
		return ids;
	}

	// Regression: the renderer used to light the scene with the first directional light in ECS storage
	// order only, silently ignoring the others.
	TEST_CASE("Every directional light is collected, the shadow caster first, then by brightness and UUID")
	{
		Scene scene;
		AddDirectionalLight(scene, 10, 8.0f, false);
		AddDirectionalLight(scene, 20, 1.0f, true);
		AddDirectionalLight(scene, 30, 2.0f, false);
		AddDirectionalLight(scene, 5, 2.0f, false);
		AddDirectionalLight(scene, 40, 3.0f, true);

		CHECK(CollectIds(scene) == std::vector<uint64_t>{ 40, 20, 10, 5, 30 });
		const std::vector<DirectionalLightInfo> lights = CollectDirectionalLights(scene);
		CHECK(lights[0].CastShadows);
		CHECK(lights[0].Radiance == glm::vec3(3.0f));
	}

	TEST_CASE("The primary light does not depend on creation order, so a play copy keeps it")
	{
		auto scene = CreateRef<Scene>();
		AddDirectionalLight(*scene, 2, 1.0f, false);
		AddDirectionalLight(*scene, 1, 1.0f, false);
		CHECK(CollectIds(*scene) == std::vector<uint64_t>{ 1, 2 });

		Ref<Scene> copy = Scene::Copy(scene);
		CHECK(CollectIds(*copy) == CollectIds(*scene));
	}

	TEST_CASE("Directions come from the world transform and are normalized; collapsed lights are skipped")
	{
		Scene scene;
		Entity parent = scene.CreateEntityWithUUID(1, "Parent");
		parent.GetTransform().SetRotationEuler({ 0.0f, glm::radians(90.0f), 0.0f });
		parent.GetTransform().Scale = glm::vec3(3.0f);
		Entity light = AddDirectionalLight(scene, 2, 1.0f, true);
		scene.SetParent(light, parent, false);
		AddDirectionalLight(scene, 3, 5.0f, true).GetTransform().Scale = glm::vec3(0.0f);

		const std::vector<DirectionalLightInfo> lights = CollectDirectionalLights(scene);
		REQUIRE(lights.size() == 1);
		CHECK(lights[0].ID == UUID(2));
		// -Z rotated 90 degrees about +Y travels along -X.
		CHECK(glm::all(glm::epsilonEqual(lights[0].Direction, glm::vec3(-1.0f, 0.0f, 0.0f), 1e-5f)));
	}

	TEST_CASE("A scene without directional lights yields none")
	{
		Scene scene;
		scene.CreateEntity("Empty");
		CHECK(CollectDirectionalLights(scene).empty());
	}
}

TEST_SUITE("DynamicResolution")
{
	using Controller = DynamicResolutionController;

	TEST_CASE("An over-budget first sample drops the scale at once to the one predicted to fit")
	{
		Controller controller;
		controller.Reset(1.0f);
		// 40 ms at full scale, 10 ms budget: cost scales with the pixel count, so aim for sqrt(9 / 40).
		controller.AddSample(40.0f, 1.0f, 10.0f, 0.25f, 1.0f);
		CHECK(controller.GetScale(0.25f, 1.0f) == doctest::Approx(std::sqrt(10.0f * Controller::Headroom / 40.0f)));

		// Samples are normalized by the scale they rendered at: 10 ms at half scale is 40 ms at full scale.
		Controller other;
		other.Reset(1.0f);
		other.AddSample(10.0f, 0.5f, 10.0f, 0.25f, 1.0f);
		CHECK(other.GetScale(0.25f, 1.0f) == doctest::Approx(controller.GetScale(0.25f, 1.0f)));
	}

	TEST_CASE("An unreachable budget pins the lowest scale exactly; the scale never leaves its bounds")
	{
		Controller controller;
		controller.Reset(1.0f);
		for (int i = 0; i < 10; i++)
			controller.AddSample(5.0f, controller.GetScale(0.5f, 1.0f), 1e-3f, 0.5f, 1.0f);
		CHECK(controller.GetScale(0.5f, 1.0f) == 0.5f);

		// The bounds move with the settings: a higher minimum or a lower maximum clamps at once.
		CHECK(controller.GetScale(0.6f, 1.0f) == 0.6f);
		CHECK(controller.GetScale(0.25f, 0.4f) == 0.4f);
		// A minimum above the maximum acts as the maximum.
		CHECK(controller.GetScale(1.5f, 0.8f) == 0.8f);
	}

	TEST_CASE("The scale rises only after RaiseDelay samples with headroom, by at most MaxRaiseStep, up to the maximum")
	{
		Controller controller;
		controller.Reset(1.0f);
		controller.AddSample(1000.0f, 1.0f, 1.0f, 0.25f, 1.0f);
		REQUIRE(controller.GetScale(0.25f, 1.0f) == 0.25f);

		// The GPU becomes fast: every sample now predicts that full scale fits.
		for (uint32_t i = 0; i + 1 < Controller::RaiseDelay; i++)
			controller.AddSample(0.01f, 0.25f, 1.0f, 0.25f, 1.0f);
		// The smoothed cost still remembers the slow frame, but RaiseDelay samples later it has decayed.
		float previous = controller.GetScale(0.25f, 1.0f);
		CHECK(previous == 0.25f);
		int raises = 0;
		for (int i = 0; i < 2000; i++)
		{
			controller.AddSample(0.01f, controller.GetScale(0.25f, 1.0f), 1.0f, 0.25f, 1.0f);
			const float scale = controller.GetScale(0.25f, 1.0f);
			CHECK(scale - previous <= Controller::MaxRaiseStep + 1e-6f);
			if (scale != previous)
				raises++;
			previous = scale;
		}
		CHECK(previous == 1.0f);
		// At least (1 - 0.25) / MaxRaiseStep steps, each RaiseDelay samples apart.
		CHECK(raises >= static_cast<int>(std::ceil(0.75f / Controller::MaxRaiseStep)));
	}

	TEST_CASE("A simulated GPU with fixed and per-pixel costs settles within the budget without oscillating")
	{
		// 2 ms of fixed work (shadow maps) plus 18 ms per full-resolution frame, results three frames late.
		auto gpuTime = [](float scale) { return 2.0f + 18.0f * scale * scale; };
		constexpr float Budget = 1000.0f / 60.0f;
		Controller controller;
		controller.Reset(1.0f);
		std::deque<float> inFlight;
		std::vector<float> scales;
		for (int frame = 0; frame < 600; frame++)
		{
			const float scale = controller.GetScale(0.25f, 1.0f);
			scales.push_back(scale);
			inFlight.push_back(scale);
			if (inFlight.size() > 3)
			{
				const float rendered = inFlight.front();
				inFlight.pop_front();
				controller.AddSample(gpuTime(rendered), rendered, Budget, 0.25f, 1.0f);
			}
		}
		const float settled = scales.back();
		CHECK(gpuTime(settled) <= Budget);
		// Not needlessly low: within a deadband-sized step of the scale that fills the headroom.
		const float ideal = std::sqrt((Budget * Controller::Headroom - 2.0f) / 18.0f);
		CHECK(settled >= ideal - 2.0f * Controller::Deadband);
		// The last 300 frames hold one scale.
		CHECK(std::all_of(scales.end() - 300, scales.end(), [&](float scale) { return scale == settled; }));
	}

	TEST_CASE("Small changes inside the deadband are ignored")
	{
		Controller controller;
		controller.Reset(1.0f);
		controller.AddSample(20.0f, 1.0f, 10.0f, 0.25f, 1.0f);
		const float scale = controller.GetScale(0.25f, 1.0f);
		// Slightly slower than predicted: the target moves by less than the deadband.
		const float target = scale * scale * 20.0f / (1.0f - Controller::Deadband * 0.5f);
		for (int i = 0; i < 100; i++)
			controller.AddSample(target * 1.01f, scale, 10.0f, 0.25f, 1.0f);
		CHECK(controller.GetScale(0.25f, 1.0f) == scale);
	}

	TEST_CASE("Invalid samples and budgets are ignored; Reset forgets the history")
	{
		constexpr float NaN = std::numeric_limits<float>::quiet_NaN();
		constexpr float Infinity = std::numeric_limits<float>::infinity();
		Controller controller;
		controller.Reset(0.8f);
		for (const float milliseconds : { 0.0f, -1.0f, NaN, Infinity })
			controller.AddSample(milliseconds, 1.0f, 1.0f, 0.25f, 1.0f);
		for (const float scale : { 0.0f, -0.5f, NaN })
			controller.AddSample(100.0f, scale, 1.0f, 0.25f, 1.0f);
		for (const float budget : { 0.0f, NaN, -Infinity })
			controller.AddSample(100.0f, 1.0f, budget, 0.25f, 1.0f);
		CHECK(controller.GetScale(0.25f, 1.0f) == 0.8f);

		controller.AddSample(100.0f, 1.0f, 1.0f, 0.25f, 1.0f);
		CHECK(controller.GetScale(0.25f, 1.0f) == 0.25f);
		controller.Reset(1.0f);
		CHECK(controller.GetScale(0.25f, 1.0f) == 1.0f);
		// After a reset the next sample is taken as is again, not blended with the old history.
		controller.AddSample(1.0f, 1.0f, 1.0f / Controller::Headroom, 0.25f, 1.0f);
		CHECK(controller.GetScale(0.25f, 1.0f) == 1.0f);
	}
}

TEST_SUITE("GpuProfiler")
{
	GpuFrameSample Sample(float frame, std::vector<float> passes)
	{
		GpuFrameSample sample;
		sample.FrameMilliseconds = frame;
		sample.PassMilliseconds = std::move(passes);
		return sample;
	}

	TEST_CASE("The timing average covers the last frames; absent passes count as zero")
	{
		GpuTimingAverage average({ "Shadows", "SSAO" }, 3);
		CHECK(average.Get().SampleCount == 0);
		REQUIRE(average.Get().Passes.size() == 2);
		CHECK(average.Get().Passes[0].Name == "Shadows");
		CHECK(average.Get().Passes[1].Milliseconds == 0.0f);

		average.Add(Sample(4.0f, { 1.0f, 2.0f }));
		average.Add(Sample(2.0f, { 1.0f, 0.0f })); // SSAO off this frame
		CHECK(average.Get().SampleCount == 2);
		CHECK(average.Get().FrameMilliseconds == doctest::Approx(3.0f));
		CHECK(average.Get().Passes[1].Milliseconds == doctest::Approx(1.0f));

		// The window drops the oldest frames.
		average.Add(Sample(6.0f, { 3.0f, 3.0f }));
		average.Add(Sample(8.0f, { 5.0f, 3.0f }));
		CHECK(average.Get().SampleCount == 3);
		CHECK(average.Get().FrameMilliseconds == doctest::Approx((2.0f + 6.0f + 8.0f) / 3.0f));
		CHECK(average.Get().Passes[0].Milliseconds == doctest::Approx(3.0f));

		average.Clear();
		CHECK(average.Get().SampleCount == 0);
		CHECK(average.Get().FrameMilliseconds == 0.0f);
	}

	TEST_CASE("A zero window still keeps the latest frame")
	{
		GpuTimingAverage average({ "Pass" }, 0);
		average.Add(Sample(1.0f, { 0.5f }));
		average.Add(Sample(3.0f, { 1.5f }));
		CHECK(average.Get().SampleCount == 1);
		CHECK(average.Get().FrameMilliseconds == 3.0f);
	}

	TEST_CASE("A disabled profiler records only markers and reports no timings")
	{
		GpuProfiler profiler(nullptr, { "Pass" }, true);
		CHECK_FALSE(profiler.IsEnabled());
		CHECK(profiler.GetTimings().SampleCount == 0);
		REQUIRE(profiler.GetTimings().Passes.size() == 1);
		CHECK(profiler.GetTimings().Passes[0].Name == "Pass");
	}

	TEST_CASE("GPU timings format for logs and render.stats")
	{
		GpuTimings timings;
		CHECK(FormatGpuTimings(timings).find("no GPU timings") != std::string::npos);
		CHECK(GpuTimingsToJson(timings)["frames"] == 0);

		timings.SampleCount = 30;
		timings.FrameMilliseconds = 2.5f;
		timings.Passes = { { "Shadows", 1.0f }, { "Lighting", 0.75f } };
		CHECK(FormatGpuTimings(timings) == "2.50 ms over 30 frames (Shadows 1.00, Lighting 0.75)");
		const nlohmann::json json = GpuTimingsToJson(timings);
		CHECK(json["frames"] == 30);
		CHECK(json["frameMs"] == 2.5);
		REQUIRE(json["passes"].size() == 2);
		CHECK(json["passes"][1]["name"] == "Lighting");
		CHECK(json["passes"][1]["ms"] == 0.75);
	}
}
