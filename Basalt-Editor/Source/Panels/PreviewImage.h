#pragma once

#include "ThumbnailManager.h"

#include <imgui.h>

namespace Basalt {

	// Draws a preview inside a size x size square at `origin`, centred and keeping its aspect ratio. Nothing
	// without a texture.
	void DrawPreviewImage(ImDrawList* drawList, const ThumbnailManager::Preview& preview, const ImVec2& origin, float size);
	// Reserves a size x size square at the cursor and draws the preview in it; a failed preview explains itself
	// in a tooltip.
	void DrawPreviewSquare(const ThumbnailManager::Preview& preview, float size);

}
