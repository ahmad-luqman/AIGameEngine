#pragma once

#include <glm/glm.hpp>

struct ImDrawList;

namespace Basalt {

	class Scene;
	class SceneRenderer;
	class GpuProfiler;

	// Draws passive play diagnostics clipped to the game image. Host timers remain separate from scene timings.
	// A null renderer reports unavailable rendering; blit is supplied only by the runtime.
	void DrawStatisticsOverlay(ImDrawList* drawList, const glm::vec2& origin, const glm::vec2& size, const Scene& scene,
							   const SceneRenderer* renderer, const GpuProfiler* host, const GpuProfiler* blit = nullptr);

}
