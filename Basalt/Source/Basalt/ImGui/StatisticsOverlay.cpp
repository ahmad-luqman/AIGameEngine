#include "Basalt/ImGui/StatisticsOverlay.h"

#include "Basalt/Renderer/PlayStatistics.h"

#include <imgui.h>

namespace Basalt {

	void DrawStatisticsOverlay(ImDrawList* drawList, const glm::vec2& origin, const glm::vec2& size, const PlayStatistics& statistics)
	{
		const std::string text = FormatStatisticsText(statistics);
		// Draw-list primitives never take focus or pointer input. Clip to the letterboxed image, including small views.
		const ImVec2 min(origin.x, origin.y);
		const ImVec2 max(origin.x + size.x, origin.y + size.y);
		drawList->PushClipRect(min, max, true);
		const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
		drawList->AddRectFilled(ImVec2(min.x + 6, min.y + 6), ImVec2(min.x + textSize.x + 22, min.y + textSize.y + 22), IM_COL32(12, 14, 18, 220), 4);
		drawList->AddText(ImVec2(min.x + 14, min.y + 14), IM_COL32(235, 240, 245, 255), text.c_str());
		drawList->PopClipRect();
	}

}
