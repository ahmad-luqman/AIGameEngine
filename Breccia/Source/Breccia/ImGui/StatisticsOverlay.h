#pragma once

#include <glm/glm.hpp>

struct ImDrawList;

namespace Breccia {

	struct PlayStatistics;

	// Draws passive play diagnostics clipped to the game image. Host timers remain separate from scene timings.
	// Formatting and data collection live in PlayStatistics and can be tested without a GPU.
	void DrawStatisticsOverlay(ImDrawList* drawList, const glm::vec2& origin, const glm::vec2& size, const PlayStatistics& statistics);

}
