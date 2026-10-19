#pragma once

#include "Basalt/Renderer/RenderUtils.h"
#include "Basalt/Renderer/SceneRenderer.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace Basalt {

	class Scene;

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

	// Failed renders are excluded so old counters cannot masquerade as a successfully rendered frame.
	PlayStatistics CollectPlayStatistics(const Scene& scene, const PlayStatisticsSources& sources);
	// Serialize the captured snapshot without re-reading live renderer/system state.
	nlohmann::json PlayStatisticsToJson(const PlayStatistics& statistics);
	// Pure text shared by the runtime and editor overlays, without ImGui dependencies.
	std::string FormatStatisticsText(const PlayStatistics& statistics);

}
