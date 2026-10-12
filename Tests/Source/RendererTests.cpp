#include <doctest/doctest.h>

#include <Basalt/Core/Log.h>
#include <Basalt/Renderer/DeviceSelection.h>
#include <Basalt/Renderer/DynamicResolution.h>
#include <Basalt/Renderer/GpuProfiler.h>
#include <Basalt/Renderer/RenderMath.h>
#include <Basalt/Renderer/RenderUtils.h>
#include <Basalt/Renderer/SceneLights.h>
#include <Basalt/Scene/Entity.h>
#include <Basalt/Scene/Scene.h>

#include <glm/gtc/epsilon.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
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
		CHECK(controller.AddSample(40.0f, 1.0f, 10.0f, 0.25f, 1.0f));
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

	TEST_CASE("Raises come RaiseDelay samples apart, by at most MaxRaiseStep, up to the maximum")
	{
		Controller controller;
		controller.Reset(1.0f);
		controller.AddSample(1000.0f, 1.0f, 1.0f, 0.25f, 1.0f);
		REQUIRE(controller.GetScale(0.25f, 1.0f) == 0.25f);

		// The GPU becomes fast: every sample now predicts that full scale fits. The smoothed cost still
		// remembers the slow frame, so raises start once it has decayed.
		std::vector<float> scales;
		for (int i = 0; i < 2000; i++)
		{
			controller.AddSample(0.01f, controller.GetScale(0.25f, 1.0f), 1.0f, 0.25f, 1.0f);
			scales.push_back(controller.GetScale(0.25f, 1.0f));
		}
		CHECK(scales.back() == 1.0f);
		float previous = 0.25f;
		size_t lastRaise = 0;
		int raises = 0;
		bool spacedAndSmall = true;
		for (size_t i = 0; i < scales.size(); i++)
		{
			if (scales[i] == previous)
				continue;
			spacedAndSmall = spacedAndSmall && scales[i] > previous && scales[i] - previous <= Controller::MaxRaiseStep + 1e-6f;
			// The first raise also waits RaiseDelay samples after the drop.
			spacedAndSmall = spacedAndSmall && i + 1 - (raises == 0 ? 0 : lastRaise + 1) >= Controller::RaiseDelay;
			lastRaise = i;
			previous = scales[i];
			raises++;
		}
		CHECK(spacedAndSmall);
		CHECK(raises >= static_cast<int>(std::ceil(0.75f / Controller::MaxRaiseStep)));
	}

	TEST_CASE("A sample without room to spare restarts the raise delay")
	{
		// At scale 0.5 with a 10 ms budget, 8 ms frames leave room for a raise, 10 ms frames do not (their target
		// is within the deadband). Without the restart the 30th sample with room would raise the scale.
		Controller controller;
		controller.Reset(0.5f);
		auto feed = [&](float milliseconds, uint32_t count) {
			for (uint32_t i = 0; i < count; i++)
				controller.AddSample(milliseconds, 0.5f, 10.0f, 0.25f, 1.0f);
		};
		feed(8.0f, Controller::RaiseDelay - 2);
		feed(10.0f, 2); // two, because the median filter ignores one
		feed(8.0f, Controller::RaiseDelay);
		CHECK(controller.GetScale(0.25f, 1.0f) == 0.5f);
		feed(8.0f, 10);
		CHECK(controller.GetScale(0.25f, 1.0f) > 0.5f);
	}

	TEST_CASE("The last step onto a bound is taken even inside the deadband")
	{
		Controller down;
		down.Reset(0.51f);
		down.AddSample(100.0f, 0.51f, 10.0f, 0.5f, 1.0f); // the target clamps to 0.5, 0.01 away
		CHECK(down.GetScale(0.5f, 1.0f) == 0.5f);

		Controller up;
		up.Reset(0.99f);
		for (uint32_t i = 0; i < Controller::RaiseDelay; i++)
			up.AddSample(0.01f, 0.99f, 10.0f, 0.25f, 1.0f);
		CHECK(up.GetScale(0.25f, 1.0f) == 1.0f);
	}

	TEST_CASE("Small changes inside the deadband are ignored, in both directions")
	{
		for (const float factor : { 1.01f, 0.99f })
		{
			INFO("factor " << factor);
			Controller controller;
			controller.Reset(1.0f);
			controller.AddSample(20.0f, 1.0f, 10.0f, 0.25f, 1.0f);
			const float scale = controller.GetScale(0.25f, 1.0f);
			// The time that makes `scale` exactly right, slightly off: the target moves by less than the deadband.
			const float exact = scale * scale * 10.0f * Controller::Headroom / (scale * scale);
			for (int i = 0; i < 100; i++)
				controller.AddSample(exact * scale * scale * factor / (scale * scale), scale, 10.0f, 0.25f, 1.0f);
			CHECK(controller.GetScale(0.25f, 1.0f) == scale);
		}
	}

	TEST_CASE("One slow frame is not a trend: the median filter ignores single spikes")
	{
		Controller controller;
		controller.Reset(1.0f);
		for (int i = 0; i < 10; i++)
			controller.AddSample(5.0f, 1.0f, 10.0f, 0.25f, 1.0f);
		REQUIRE(controller.GetScale(0.25f, 1.0f) == 1.0f);
		controller.AddSample(500.0f, 1.0f, 10.0f, 0.25f, 1.0f); // a hitch (shader compile, asset streaming)
		controller.AddSample(5.0f, 1.0f, 10.0f, 0.25f, 1.0f);
		controller.AddSample(5.0f, 1.0f, 10.0f, 0.25f, 1.0f);
		CHECK(controller.GetScale(0.25f, 1.0f) == 1.0f);

		// Two slow frames in a row are.
		controller.AddSample(500.0f, 1.0f, 10.0f, 0.25f, 1.0f);
		controller.AddSample(500.0f, 1.0f, 10.0f, 0.25f, 1.0f);
		CHECK(controller.GetScale(0.25f, 1.0f) < 1.0f);
	}

	TEST_CASE("A simulated GPU with fixed and per-pixel costs settles within the budget without oscillating")
	{
		// 2 ms of fixed work (shadow maps) plus 18 ms per full-resolution frame. A frame's result reaches the
		// controller GpuProfiler::Latency frames after it was queued, as with the real profiler.
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
			if (inFlight.size() > GpuProfiler::Latency - 1)
			{
				const float rendered = inFlight.front();
				inFlight.pop_front();
				controller.AddSample(gpuTime(rendered), rendered, Budget, 0.25f, 1.0f);
			}
		}
		const float settled = scales.back();
		CHECK(gpuTime(settled) <= Budget);
		// Not needlessly low: within two deadbands of the scale that fills the headroom.
		const float ideal = std::sqrt((Budget * Controller::Headroom - 2.0f) / 18.0f);
		CHECK(settled >= ideal - 2.0f * Controller::Deadband);
		// The last 300 frames hold one scale.
		CHECK(std::all_of(scales.end() - 300, scales.end(), [&](float scale) { return scale == settled; }));
	}

	TEST_CASE("Invalid samples and budgets are ignored; Reset forgets the history, ForgetHistory keeps the scale")
	{
		constexpr float NaN = std::numeric_limits<float>::quiet_NaN();
		constexpr float Infinity = std::numeric_limits<float>::infinity();
		Controller controller;
		controller.Reset(0.8f);
		for (const float milliseconds : { 0.0f, -1.0f, NaN, Infinity })
			CHECK_FALSE(controller.AddSample(milliseconds, 1.0f, 1.0f, 0.25f, 1.0f));
		for (const float scale : { 0.0f, -0.5f, NaN })
			CHECK_FALSE(controller.AddSample(100.0f, scale, 1.0f, 0.25f, 1.0f));
		for (const float budget : { 0.0f, NaN, -Infinity })
			CHECK_FALSE(controller.AddSample(100.0f, 1.0f, budget, 0.25f, 1.0f));
		CHECK(controller.GetScale(0.25f, 1.0f) == 0.8f);

		controller.AddSample(100.0f, 1.0f, 1.0f, 0.25f, 1.0f);
		CHECK(controller.GetScale(0.25f, 1.0f) == 0.25f);
		controller.ForgetHistory();
		CHECK(controller.GetScale(0.25f, 1.0f) == 0.25f);
		controller.Reset(1.0f);
		CHECK(controller.GetScale(0.25f, 1.0f) == 1.0f);
		// After a reset the next sample is taken as is again, not blended with the old history.
		controller.AddSample(1.0f, 1.0f, 1.0f / Controller::Headroom, 0.25f, 1.0f);
		CHECK(controller.GetScale(0.25f, 1.0f) == 1.0f);
	}

	TEST_CASE("Settings are sanitized for programmatic callers")
	{
		constexpr float NaN = std::numeric_limits<float>::quiet_NaN();
		const DynamicResolutionSettings defaults;
		CHECK(SanitizeDynamicResolution(defaults) == defaults);
		CHECK(SanitizeDynamicResolution({ true, NaN, NaN }) == DynamicResolutionSettings{ true, defaults.TargetFrameRate, defaults.MinScale });
		CHECK(SanitizeDynamicResolution({ true, 0.0f, 0.0f }) == DynamicResolutionSettings{ true, MinTargetFrameRate, MinRenderScale });
		CHECK(SanitizeDynamicResolution({ true, std::numeric_limits<float>::infinity(), 5.0f }) == DynamicResolutionSettings{ true, MinTargetFrameRate, MaxRenderScale });
		// Rates above the project range stay (the runtime's test affordance).
		CHECK(SanitizeDynamicResolution({ true, 1e6f, 0.5f }).TargetFrameRate == 1e6f);
	}

	TEST_CASE("Dynamic render sizes stay inside the targets and grow with the scale")
	{
		const glm::uvec2 target = ComputeRenderSize(1281, 721, 1.0f);
		glm::uvec2 previous(0);
		for (int percent = 25; percent <= 100; percent++)
		{
			const float scale = static_cast<float>(percent) / 100.0f;
			const glm::uvec2 size = ComputeDynamicRenderSize(1281, 721, scale, target);
			CHECK(size.x <= target.x);
			CHECK(size.y <= target.y);
			CHECK(size.x >= previous.x);
			CHECK(size.y >= previous.y);
			previous = size;
		}
		CHECK(ComputeDynamicRenderSize(1281, 721, 0.5f, target) == glm::uvec2(641, 361));
		// A scale above the one the targets were made for is clipped to them.
		CHECK(ComputeDynamicRenderSize(1281, 721, 2.0f, target) == target);
	}

	TEST_CASE("The pixel scale is the square root of the pixel ratio")
	{
		CHECK(ComputePixelScale({ 640, 360 }, { 1280, 720 }) == doctest::Approx(0.5f));
		CHECK(ComputePixelScale({ 1280, 720 }, { 1280, 720 }) == 1.0f);
		CHECK(ComputePixelScale({ 641, 361 }, { 1281, 721 }) == doctest::Approx(std::sqrt(641.0f * 361.0f / (1281.0f * 721.0f))));
		CHECK(std::isfinite(ComputePixelScale({ 1, 1 }, { 0, 0 })));
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

		// Dropped frames are counted for the profiler's lifetime, outside the window.
		CHECK(average.Get().DroppedFrames == 0);
		average.AddDropped();
		average.AddDropped();
		CHECK(average.Get().DroppedFrames == 2);
		CHECK(average.Get().SampleCount == 3);
	}

	TEST_CASE("A zero window still keeps the latest frame")
	{
		GpuTimingAverage average({ "Pass" }, 0);
		average.Add(Sample(1.0f, { 0.5f }));
		average.Add(Sample(3.0f, { 1.5f }));
		CHECK(average.Get().SampleCount == 1);
		CHECK(average.Get().FrameMilliseconds == 3.0f);
	}

	TEST_CASE("Scale-independent passes can be left out of a frame's time")
	{
		const GpuFrameSample sample = Sample(10.0f, { 4.0f, 1.0f, 2.0f });
		CHECK(sample.MillisecondsWithout({ 0, 1 }) == doctest::Approx(5.0f));
		CHECK(sample.MillisecondsWithout({}) == 10.0f);
		// Unknown passes are ignored rather than read out of bounds.
		CHECK(sample.MillisecondsWithout({ 7 }) == 10.0f);
	}

	TEST_CASE("A disabled profiler records only markers and reports no timings")
	{
		GpuProfiler profiler(nullptr, { "Pass" }, true);
		CHECK_FALSE(profiler.IsEnabled());
		CHECK(profiler.GetTimings().SampleCount == 0);
		REQUIRE(profiler.GetTimings().Passes.size() == 1);
		CHECK(profiler.GetTimings().Passes[0].Name == "Pass");
	}

	TEST_CASE("GPU timings format for logs and render.stats, saying why there are none")
	{
		GpuTimings timings;
		CHECK(FormatGpuTimings(timings, false).find("no GPU timer queries") != std::string::npos);
		timings.DroppedFrames = 4;
		CHECK(FormatGpuTimings(timings, true).find("no GPU timer results arrived (4 frames dropped)") != std::string::npos);
		CHECK(GpuTimingsToJson(timings)["frames"] == 0);
		CHECK(GpuTimingsToJson(timings)["droppedFrames"] == 4);

		timings.SampleCount = 30;
		timings.FrameMilliseconds = 2.5f;
		timings.Passes = { { "Shadows", 1.0f }, { "Lighting", 0.75f } };
		CHECK(FormatGpuTimings(timings, true) == "2.50 ms over 30 frames (Shadows 1.00, Lighting 0.75)");
		const nlohmann::json json = GpuTimingsToJson(timings);
		CHECK(json["frames"] == 30);
		CHECK(json["frameMs"] == 2.5);
		REQUIRE(json["passes"].size() == 2);
		CHECK(json["passes"][1]["name"] == "Lighting");
		CHECK(json["passes"][1]["ms"] == 0.75);
	}
}

TEST_SUITE("SceneRenderer")
{
	int CountMessages(uint64_t since, const std::string& text)
	{
		uint64_t next = 0;
		int count = 0;
		for (const LogMessage& message : Log::GetHistory().GetMessagesSince(since, next))
			count += message.Text.find(text) != std::string::npos ? 1 : 0;
		return count;
	}

	// A renderer without a device (never initialized) has no GPU timers, which is enough to check the
	// dynamic resolution state and the render.stats contract headless.
	TEST_CASE("Without GPU timers dynamic resolution stays inactive, says so once, and render.stats reports it")
	{
		const uint64_t since = Log::GetHistory().GetTotalCount();
		SceneRenderer renderer(nullptr, SceneRendererSpecification{ true });
		CHECK_FALSE(renderer.HasGpuTimers());
		renderer.SetDynamicResolution({ true, 30.0f, 0.5f });
		renderer.SetDynamicResolution({ true, 60.0f, 0.5f });
		CHECK(renderer.GetDynamicResolution().Enabled);
		CHECK_FALSE(renderer.IsDynamicResolutionActive());
		CHECK(CountMessages(since, "dynamic resolution needs GPU timer queries") == 1);

		const nlohmann::json stats = GetRenderStats(renderer);
		CHECK(stats["dynamicResolution"] == true);
		CHECK(stats["dynamicResolutionActive"] == false);
		CHECK(stats["targetFrameRate"] == 60.0);
		CHECK(stats["minRenderScale"] == 0.5);
		CHECK(stats["gpu"]["timers"] == false);
		CHECK(stats["gpu"]["frames"] == 0);
		CHECK(stats["gpu"]["droppedFrames"] == 0);
		CHECK(stats["gpu"]["passes"].size() == 9);
		CHECK(stats["gpu"]["passes"][0]["name"] == "Environment");
		for (const char* key : { "viewport", "renderSize", "renderScale", "currentRenderScale", "drawCalls", "shadowDrawCalls", "culledSubmeshes", "lights", "materials" })
		{
			INFO(key);
			CHECK(stats.contains(key));
		}
	}

	TEST_CASE("Dynamic resolution settings are sanitized by the renderer")
	{
		SceneRenderer renderer(nullptr);
		renderer.SetDynamicResolution({ true, std::numeric_limits<float>::quiet_NaN(), 9.0f });
		CHECK(renderer.GetDynamicResolution() == DynamicResolutionSettings{ true, 60.0f, MaxRenderScale });
	}
}
