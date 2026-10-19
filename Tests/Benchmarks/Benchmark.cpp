#include "BenchmarkScene.h"

#include "Basalt/Animation/AnimationSystem.h"
#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Core/JobSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/Timer.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Renderer/GraphicsDevice.h"
#include "Basalt/Renderer/RenderUtils.h"
#include "Basalt/Renderer/VulkanLoader.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <string_view>

using namespace Basalt;

int main(int argc, char** argv)
{
	Log::Init();
	Log::SetConsoleLevel(LogLevel::Warn);
	if (argc != 3 || (std::string_view(argv[1]) != "cpu" && std::string_view(argv[1]) != "gpu"))
	{
		BS_CORE_ERROR("Usage: BasaltBenchmark cpu|gpu report.json");
		return 1;
	}
	const bool gpu = std::string_view(argv[1]) == "gpu";
	Scope<GraphicsDevice> graphics;
	Scope<SceneRenderer> renderer;
	nvrhi::CommandListHandle commandList;
	if (gpu)
	{
		if (!VulkanLoader::Load())
			return 1;
		graphics = GraphicsDevice::CreateOffscreen(320, 180, { .ApplicationName = "BasaltBenchmark", .EnableValidation = true, .VSync = false });
		if (!graphics || !graphics->IsValidationActive() || !graphics->SupportsTimerQueries())
		{
			BS_CORE_ERROR("Benchmark requires Vulkan validation and GPU timer queries");
			return 1;
		}
		renderer = CreateScope<SceneRenderer>(graphics->GetDevice(), SceneRendererSpecification{ .GpuTimers = true });
		if (!renderer->Initialize())
			return 1;
		renderer->SetViewportSize(320, 180);
		commandList = graphics->GetDevice()->createCommandList();
		if (!commandList)
			return 1;
	}
	Scene scene;
	BasaltBenchmark::BuildScene(scene);
	scene.OnViewportResize(320, 180);
	scene.OnRuntimeStart();
	const auto camera = GetPrimaryRenderCamera(scene, 320, 180);
	if (!camera || !BasaltBenchmark::CountWorkload(scene).IsComplete())
	{
		BS_CORE_ERROR("Benchmark workload failed to initialize");
		return 1;
	}
	uint32_t playing = 0;
	for (auto entity : scene.GetAllEntitiesOrdered())
		if (entity.HasComponent<AnimatorComponent>() && scene.GetAnimationSystem()->IsPlaying(entity))
			++playing;
	if (playing != BasaltBenchmark::Animators)
	{
		BS_CORE_ERROR("Benchmark expected {} playing animators, got {}", BasaltBenchmark::Animators, playing);
		return 1;
	}
	nlohmann::json samples = nlohmann::json::array();
	for (uint32_t frame = 0; frame < BasaltBenchmark::WarmupFrames + BasaltBenchmark::SampleFrames; ++frame)
	{
		Timer timer;
		scene.OnUpdate(BasaltBenchmark::FixedStep);
		if (gpu)
		{
			if (!graphics->BeginFrame())
				return 1;
			renderer->Render(scene, *camera, BasaltBenchmark::FixedStep);
			if (renderer->GetStatistics().Failed)
				return 1;
			commandList->open();
			renderer->Blit(commandList, graphics->GetCurrentFramebuffer());
			commandList->close();
			graphics->GetDevice()->executeCommandList(commandList);
			graphics->Present();
			// Complete-frame latency, including the GPU, rather than how fast the CPU can queue work.
			graphics->WaitIdle();
		}
		if (frame >= BasaltBenchmark::WarmupFrames)
			samples.push_back(timer.ElapsedMillis());
	}
	const auto counts = BasaltBenchmark::CountWorkload(scene);
	if (!counts.IsComplete())
	{
		BS_CORE_ERROR("Benchmark workload changed during the run");
		return 1;
	}
	nlohmann::json report = {
		{ "version", 1 },
		{ "buildType", BASALT_BENCHMARK_BUILD_TYPE },
		{ "compiler", BASALT_BENCHMARK_COMPILER },
		{ "mode", argv[1] },
		{ "warmupFrames", BasaltBenchmark::WarmupFrames },
		{ "fixedStep", BasaltBenchmark::FixedStep },
		{ "samplesMs", samples },
		{ "entities", counts.Entities },
		{ "meshes", counts.Meshes },
		{ "animators", counts.Animators },
		{ "lights", counts.Lights },
		{ "physicsBodies", counts.Bodies },
		{ "workers", JobSystem::Get().GetWorkerCount() },
		{ "adapter", graphics ? graphics->GetAdapterName() : "headless" },
	};
	if (gpu)
	{
		report["renderStats"] = GetRenderStats(*renderer);
		std::string error;
		if (!SaveScreenshot(*renderer, std::string(argv[2]) + ".png", error))
		{
			BS_CORE_ERROR("Benchmark screenshot: {}", error);
			return 1;
		}
		if (graphics->GetValidationErrorCount() || !renderer->GetGpuTimings().SampleCount || renderer->GetStatistics().Triangles < static_cast<uint64_t>(BasaltBenchmark::Meshes) * 12 || renderer->GetStatistics().SkinMatrices == 0 || renderer->GetStatistics().Lights != BasaltBenchmark::Lights + 1)
		{
			BS_CORE_ERROR("Benchmark render failed validation, GPU timing or visible geometry checks");
			return 1;
		}
	}
	scene.OnRuntimeStop();
	JobSystem::Get().WaitIdle();
	AssetManager::Clear();
	if (Log::GetHistory().GetErrorCount())
		return 1;
	std::ofstream output(argv[2]);
	output << report.dump(2) << '\n';
	output.close();
	if (!output)
	{
		BS_CORE_ERROR("Cannot write benchmark report to '{}'", argv[2]);
		return 1;
	}
	return 0;
}
