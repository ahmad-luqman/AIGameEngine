#pragma once

#include "Basalt/Asset/ImageUtils.h"
#include "Basalt/Renderer/SceneRenderer.h"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	class Scene;

	// A copied GPU timing status; missing, disabled, pending and stalled are distinct from a measured zero.
	struct GpuStatistics
	{
		GpuTimings Timings;
		// A reason means disabled; null means queries are enabled (possibly pending or stalled).
		std::optional<std::string> DisabledReason = "GPU timers disabled or unsupported";
		uint32_t ConsecutiveDrops = 0;

		bool HasTimers() const { return !DisabledReason; }
		bool IsStalled() const { return ConsecutiveDrops >= GpuProfiler::DropWarningThreshold; }
		bool IsReady() const { return HasTimers() && Timings.SampleCount > 0 && !IsStalled(); }
	};

	// Snapshot the profiler once so logs, overlays and automation share availability rules.
	std::optional<GpuStatistics> CollectGpuStatistics(const GpuProfiler* profiler);
	// Unready measurements are null; counts/status remain available for diagnosing missing results.
	nlohmann::json GpuStatisticsToJson(const std::optional<GpuStatistics>& statistics);
	// Overlays list passes separately; bounded-run logs request full pass details on one line.
	std::string FormatGpuStatistics(const std::optional<GpuStatistics>& statistics, bool includePasses = false);

	// Camera for the scene's primary CameraComponent (aspect ratio from the viewport), if any.
	std::optional<RenderCamera> GetPrimaryRenderCamera(Scene& scene, uint32_t viewportWidth, uint32_t viewportHeight);

	// Writes the renderer's last output image as a PNG; rejects a failed frame before readback.
	bool SaveScreenshot(SceneRenderer& renderer, const std::filesystem::path& path, std::string& outError);

	// Raw rolling average serialization; public diagnostics use GpuStatisticsToJson to mask unready values.
	nlohmann::json GpuTimingsToJson(const GpuTimings& timings);
	// One line for logs: "2.41 ms over 30 frames (Environment 0.00, Uploads 0.05, Shadows 0.52, ...)" (plus the
	// dropped frames, if any), or why there are none: "unavailable (...)" without timer queries, "not available
	// yet (...)" when no results arrived. FormatGpuStatistics adds live stall/disable status; GPU tests
	// reject unavailable, pending and stalled prefixes.
	std::string FormatGpuTimings(const GpuTimings& timings, bool timersAvailable);
	// Everything render.stats reports about a renderer's last frames. The dynamic resolution keys match
	// project.info's.
	nlohmann::json GetRenderStats(const SceneRenderer& renderer);
	// Reuse an already captured GPU snapshot when gathering a larger diagnostic report.
	nlohmann::json GetRenderStats(const SceneRenderer& renderer, const std::optional<GpuStatistics>& gpu);

}
