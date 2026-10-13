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

	// Writes the renderer's last output image as a PNG; rejects a failed frame before readback.
	bool SaveScreenshot(SceneRenderer& renderer, const std::filesystem::path& path, std::string& outError);

	// { "frames", "droppedFrames", "frameMs", "passes": [{ "name", "ms" }] } (render.stats).
	nlohmann::json GpuTimingsToJson(const GpuTimings& timings);
	// One line for logs: "2.41 ms over 30 frames (Environment 0.00, Uploads 0.05, Shadows 0.52, ...)" (plus the
	// dropped frames, if any), or why there are none: "unavailable (...)" without timer queries, "not available
	// yet (...)" when no results arrived. Tests/CMakeLists.txt fails GPU runs on those two prefixes.
	std::string FormatGpuTimings(const GpuTimings& timings, bool timersAvailable);
	// Everything render.stats reports about a renderer's last frames. The dynamic resolution keys match
	// project.info's.
	nlohmann::json GetRenderStats(const SceneRenderer& renderer);

}
