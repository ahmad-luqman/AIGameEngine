#pragma once

#include "Basalt/Renderer/GpuProfiler.h"
#include "Basalt/Renderer/SceneRenderer.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace Basalt {

	class Scene;

	// A copied GPU timing status; missing, disabled, pending and stalled are distinct from a measured zero.
	struct GpuStatistics
	{
		GpuTimings Timings;
		bool Timers = false;
		uint32_t ConsecutiveDrops = 0;
		std::string DisabledReason;

		bool IsStalled() const { return ConsecutiveDrops >= GpuProfiler::DropWarningThreshold; }
		bool IsReady() const { return Timers && Timings.SampleCount > 0 && !IsStalled(); }
	};

	// Named sources prevent mixing the ImGui and runtime blit profilers. Renderer is null for an unrendered frame.
	struct PlayStatisticsSources
	{
		std::optional<float> FrameMilliseconds;
		const SceneRenderer* Renderer = nullptr;
		const GpuProfiler* ImGui = nullptr;
		const GpuProfiler* Blit = nullptr;
	};

	// A single snapshot used by the overlay and automation, independent of ImGui and Application globals.
	struct PlayStatistics
	{
		std::optional<float> FrameMilliseconds;
		std::optional<uint32_t> PhysicsBodies;
		uint64_t LuaBytes = 0;
		std::optional<SceneRendererStatistics> Renderer;
		// Full render.stats settings/counters captured along with Renderer; null when the frame did not render.
		nlohmann::json RenderDetails;
		std::optional<GpuStatistics> SceneGpu;
		std::optional<GpuStatistics> ImGuiGpu;
		std::optional<GpuStatistics> BlitGpu;
	};

	std::optional<GpuStatistics> CollectGpuStatistics(const GpuProfiler* profiler);
	PlayStatistics CollectPlayStatistics(const Scene& scene, const PlayStatisticsSources& sources);
	nlohmann::json GpuStatisticsToJson(const std::optional<GpuStatistics>& statistics);
	nlohmann::json PlayStatisticsToJson(const PlayStatistics& statistics);
	std::string FormatGpuStatistics(const std::optional<GpuStatistics>& statistics);
	std::string FormatStatisticsText(const PlayStatistics& statistics);

}
