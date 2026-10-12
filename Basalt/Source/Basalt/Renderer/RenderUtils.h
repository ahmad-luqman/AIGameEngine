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

	// Camera for the scene's primary CameraComponent (aspect ratio from the viewport), if any.
	std::optional<RenderCamera> GetPrimaryRenderCamera(Scene& scene, uint32_t viewportWidth, uint32_t viewportHeight);

	// Writes the renderer's last output image as a PNG.
	bool SaveScreenshot(SceneRenderer& renderer, const std::filesystem::path& path, std::string& outError);

	// { "frames", "droppedFrames", "frameMs", "passes": [{ "name", "ms" }] } (render.stats).
	nlohmann::json GpuTimingsToJson(const GpuTimings& timings);
	// One line for logs: "2.41 ms over 30 frames (Environment 0.00, Uploads 0.05, Shadows 0.52, ...)", or why
	// there are no timings: no timer queries (timersAvailable false), or no results arrived (with the number
	// of dropped frames).
	std::string FormatGpuTimings(const GpuTimings& timings, bool timersAvailable);
	// Everything render.stats reports about a renderer's last frames. The dynamic resolution keys match
	// project.info's.
	nlohmann::json GetRenderStats(const SceneRenderer& renderer);

}
