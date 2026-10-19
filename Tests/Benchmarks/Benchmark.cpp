#include "BenchmarkScene.h"

#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/JobSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/Timer.h"
#include "Basalt/Renderer/GraphicsDevice.h"
#include "Basalt/Renderer/RenderUtils.h"
#include "Basalt/Renderer/VulkanLoader.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <string_view>

using namespace Basalt;

namespace {
	// Unwind runtime state and cached assets before releasing the device, including on a failed run.
	struct BenchmarkSession
	{
		Scope<GraphicsDevice> Graphics;
		Scope<SceneRenderer> Renderer;
		nvrhi::CommandListHandle CommandList;
		Scene World;

		~BenchmarkSession()
		{
			World.OnRuntimeStop();
			JobSystem::Get().WaitIdle();
			if (Graphics)
				Graphics->WaitIdle();
			AssetManager::Clear();
		}
	};

	bool RunBenchmark(bool gpu, const std::string& path, nlohmann::json& report, std::string& error)
	{
		using namespace BasaltBenchmark;
		auto fail = [&](const std::string& reason) {
			error = reason;
			BS_CORE_ERROR("Benchmark: {}", reason);
			return false;
		};
		BenchmarkSession session;
		auto& graphics = session.Graphics;
		auto& renderer = session.Renderer;
		auto& commandList = session.CommandList;
		auto& scene = session.World;
		if (gpu)
		{
			if (!VulkanLoader::Load())
				return fail("Vulkan loader initialization failed");
			graphics = GraphicsDevice::CreateOffscreen(Width, Height, { .ApplicationName = "BasaltBenchmark", .EnableValidation = true, .VSync = false });
			if (!graphics)
				return fail("offscreen graphics device creation failed");
			if (!graphics->IsValidationActive())
				return fail("Vulkan validation is required but inactive");
			if (!graphics->SupportsTimerQueries())
				return fail("GPU timer queries are required but unavailable");
			renderer = CreateScope<SceneRenderer>(graphics->GetDevice(), SceneRendererSpecification{ .GpuTimers = true });
			if (!renderer->Initialize())
				return fail("scene renderer initialization failed");
			renderer->SetViewportSize(Width, Height);
			commandList = graphics->GetDevice()->createCommandList();
			if (!commandList)
				return fail("blit command list creation failed");
		}
		BuildScene(scene);
		scene.OnViewportResize(Width, Height);
		scene.OnRuntimeStart();
		const auto camera = GetPrimaryRenderCamera(scene, Width, Height);
		if (!camera)
			return fail("benchmark primary camera is unavailable");
		if (!CountWorkload(scene).IsComplete())
			return fail("benchmark component/body workload failed to initialize");
		const auto playing = CountPlayingAnimators(scene);
		if (playing != Animators)
			return fail(fmt::format("playing animators: expected {}, got {}", Animators, playing));
		report["samplesMs"] = nlohmann::json::array();
		for (uint32_t frame = 0; frame < WarmupFrames + SampleFrames; ++frame)
		{
			Timer timer;
			scene.OnUpdate(FixedStep);
			if (gpu)
			{
				if (!graphics->BeginFrame())
					return fail(fmt::format("offscreen BeginFrame failed at frame {}", frame));
				renderer->Render(scene, *camera, FixedStep);
				if (renderer->GetStatistics().Failed)
					return fail(fmt::format("scene rendering failed at frame {}", frame));
				commandList->open();
				renderer->Blit(commandList, graphics->GetCurrentFramebuffer());
				commandList->close();
				graphics->GetDevice()->executeCommandList(commandList);
				graphics->Present();
				// Complete-frame latency, including the GPU, rather than how fast the CPU can queue work.
				graphics->WaitIdle();
			}
			if (frame >= WarmupFrames)
				report["samplesMs"].push_back(timer.ElapsedMillis());
		}
		const auto counts = CountWorkload(scene);
		report.update({ { "entities", counts.Entities }, { "meshes", counts.Meshes }, { "animators", counts.Animators }, { "lights", counts.Lights }, { "physicsBodies", counts.Bodies }, { "workers", JobSystem::Get().GetWorkerCount() }, { "adapter", graphics ? graphics->GetAdapterName() : "headless" } });
		if (!counts.IsComplete())
			return fail("benchmark component/body workload changed during the run");
		if (gpu)
		{
			report["renderStats"] = GetRenderStats(*renderer);
			if (const auto errors = graphics->GetValidationErrorCount())
				return fail(fmt::format("{} GPU validation errors", errors));
			const auto& timings = renderer->GetGpuTimings();
			if (!timings.SampleCount || !std::isfinite(timings.FrameMilliseconds) || timings.FrameMilliseconds <= 0)
				return fail(fmt::format("GPU timing unavailable or invalid: {} samples, {} ms", timings.SampleCount, timings.FrameMilliseconds));
			if (const auto problem = ValidateRenderedWorkload(renderer->GetStatistics()); !problem.empty())
				return fail(problem);
			std::string screenshotError;
			if (!SaveScreenshot(*renderer, path + ".png", screenshotError))
				return fail("screenshot failed: " + screenshotError);
		}
		return true;
	}
}

int main(int argc, char** argv)
{
	Log::Init();
	Log::SetConsoleLevel(LogLevel::Warn);
	if (argc != 3 || (std::string_view(argv[1]) != "cpu" && std::string_view(argv[1]) != "gpu"))
	{
		BS_CORE_ERROR("Usage: BasaltBenchmark cpu|gpu report.json");
		return 1;
	}
	nlohmann::json report = {
		{ "version", 1 }, { "buildType", BASALT_BENCHMARK_BUILD_TYPE }, { "compiler", BASALT_BENCHMARK_COMPILER }, { "mode", argv[1] }, { "warmupFrames", BasaltBenchmark::WarmupFrames }, { "fixedStep", BasaltBenchmark::FixedStep }
	};
	std::string error;
	bool passed = RunBenchmark(std::string_view(argv[1]) == "gpu", argv[2], report, error);
	if (const auto errors = Log::GetHistory().GetErrorCount(); passed && errors)
	{
		error = fmt::format("{} engine errors were logged; successful report refused", errors);
		BS_CORE_ERROR("Benchmark: {}", error);
		passed = false;
	}
	report["passed"] = passed;
	if (!passed)
		report["failure"] = error;
	std::ofstream output(argv[2]);
	output << report.dump(2) << '\n';
	output.close();
	if (!output)
	{
		BS_CORE_ERROR("Benchmark: cannot write report to '{}'", argv[2]);
		return 1;
	}
	return passed ? 0 : 1;
}
