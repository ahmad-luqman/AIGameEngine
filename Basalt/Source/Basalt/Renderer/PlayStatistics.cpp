#include "Basalt/Renderer/PlayStatistics.h"

#include "Basalt/Core/MemoryTracker.h"
#include "Basalt/Physics/PhysicsWorld.h"
#include "Basalt/Renderer/RenderUtils.h"
#include "Basalt/Scene/Scene.h"

#include <spdlog/fmt/fmt.h>

namespace Basalt {

	std::optional<GpuStatistics> CollectGpuStatistics(const GpuProfiler* profiler)
	{
		if (!profiler)
			return std::nullopt;
		return GpuStatistics{ profiler->GetTimings(), profiler->IsEnabled(), profiler->GetConsecutiveDrops(), profiler->GetDisabledReason() };
	}

	PlayStatistics CollectPlayStatistics(const Scene& scene, const PlayStatisticsSources& sources)
	{
		PlayStatistics statistics;
		statistics.FrameMilliseconds = sources.FrameMilliseconds;
		if (const auto* physics = scene.GetPhysicsWorld())
			statistics.PhysicsBodies = physics->GetBodyCount();
		statistics.LuaBytes = MemoryTracker::Get(MemoryCategory::Lua).Bytes;
		if (sources.Renderer && !sources.Renderer->GetStatistics().Failed)
		{
			statistics.Renderer = sources.Renderer->GetStatistics();
			statistics.RenderDetails = GetRenderStats(*sources.Renderer);
			statistics.SceneGpu = CollectGpuStatistics(&sources.Renderer->GetGpuProfiler());
		}
		statistics.ImGuiGpu = CollectGpuStatistics(sources.ImGui);
		statistics.BlitGpu = CollectGpuStatistics(sources.Blit);
		return statistics;
	}

	nlohmann::json GpuStatisticsToJson(const std::optional<GpuStatistics>& statistics)
	{
		if (!statistics)
			return nullptr;
		auto result = GpuTimingsToJson(statistics->Timings);
		result["timers"] = statistics->Timers;
		result["consecutiveDrops"] = statistics->ConsecutiveDrops;
		result["stalled"] = statistics->IsStalled();
		result["disabledReason"] = statistics->Timers ? nlohmann::json() : nlohmann::json(statistics->DisabledReason);
		return result;
	}

	nlohmann::json PlayStatisticsToJson(const PlayStatistics& statistics)
	{
		auto render = statistics.RenderDetails;
		if (!render.is_null())
			render["gpu"] = GpuStatisticsToJson(statistics.SceneGpu);
		return {
			{ "frameMs", statistics.FrameMilliseconds ? nlohmann::json(*statistics.FrameMilliseconds) : nlohmann::json() },
			{ "physicsBodies", statistics.PhysicsBodies ? nlohmann::json(*statistics.PhysicsBodies) : nlohmann::json() },
			{ "luaBytes", statistics.LuaBytes },
			{ "renderStats", render },
			{ "imguiGpu", GpuStatisticsToJson(statistics.ImGuiGpu) },
			{ "blitGpu", GpuStatisticsToJson(statistics.BlitGpu) }
		};
	}

	std::string FormatGpuStatistics(const std::optional<GpuStatistics>& statistics)
	{
		if (!statistics)
			return "unavailable";
		if (!statistics->Timers)
			return "unavailable (" + statistics->DisabledReason + ")";
		if (statistics->IsStalled())
			return fmt::format("stalled ({} consecutive frames without results)", statistics->ConsecutiveDrops);
		if (!statistics->Timings.SampleCount)
			return "n/a (waiting for GPU timings)";
		return fmt::format("{:.2f} ms ({} samples, {} dropped)", statistics->Timings.FrameMilliseconds, statistics->Timings.SampleCount, statistics->Timings.DroppedFrames);
	}

	std::string FormatStatisticsText(const PlayStatistics& statistics)
	{
		std::string text = statistics.FrameMilliseconds
							   ? fmt::format("Frame {:.2f} ms ({:.0f} FPS)\n", *statistics.FrameMilliseconds, 1000.0f / *statistics.FrameMilliseconds)
							   : "Frame n/a (waiting for a completed frame)\n";
		text += fmt::format("Physics bodies {}  |  Lua {:.2f} MiB (process)\n",
							statistics.PhysicsBodies ? std::to_string(*statistics.PhysicsBodies) : "n/a", static_cast<double>(statistics.LuaBytes) / 1048576.0);
		if (statistics.Renderer)
		{
			const auto& renderer = *statistics.Renderer;
			text += fmt::format("Draw calls {} (+{} shadow)  |  Triangles {}\n", renderer.DrawCalls, renderer.ShadowDrawCalls, renderer.Triangles);
			text += "Game GPU: " + FormatGpuStatistics(statistics.SceneGpu) + '\n';
			if (statistics.SceneGpu && statistics.SceneGpu->IsReady())
				for (const auto& pass : statistics.SceneGpu->Timings.Passes)
					text += fmt::format("  {}: {:.2f} ms\n", pass.Name, pass.Milliseconds);
		}
		else
			text += "Game rendering: unavailable\n";
		if (statistics.Renderer && statistics.SceneGpu && statistics.SceneGpu->IsReady() && statistics.ImGuiGpu && statistics.ImGuiGpu->IsReady() && (!statistics.BlitGpu || statistics.BlitGpu->IsReady()))
		{
			const float total = statistics.SceneGpu->Timings.FrameMilliseconds + statistics.ImGuiGpu->Timings.FrameMilliseconds + (statistics.BlitGpu ? statistics.BlitGpu->Timings.FrameMilliseconds : 0.0f);
			text += fmt::format("Game + host GPU: {:.2f} ms\n", total);
		}
		else
		{
			const bool unavailable = !statistics.Renderer || !statistics.SceneGpu || !statistics.SceneGpu->Timers || !statistics.ImGuiGpu || !statistics.ImGuiGpu->Timers || (statistics.BlitGpu && !statistics.BlitGpu->Timers);
			const bool stalled = (statistics.SceneGpu && statistics.SceneGpu->IsStalled()) || (statistics.ImGuiGpu && statistics.ImGuiGpu->IsStalled()) || (statistics.BlitGpu && statistics.BlitGpu->IsStalled());
			text += fmt::format("Game + host GPU: n/a ({})\n", unavailable ? "GPU timings unavailable" : stalled ? "GPU timings stalled"
																												 : "waiting for GPU timings");
		}
		text += "ImGui GPU: " + FormatGpuStatistics(statistics.ImGuiGpu) + '\n';
		if (statistics.BlitGpu)
			text += "Runtime blit GPU: " + FormatGpuStatistics(statistics.BlitGpu) + '\n';
		return text;
	}

}
