#include "Panels/PreviewImage.h"

#include <algorithm>

namespace Basalt {

	void DrawPreviewImage(ImDrawList* drawList, const ThumbnailManager::Preview& preview, const ImVec2& origin, float size)
	{
		if (!preview.Texture || preview.Width == 0 || preview.Height == 0)
			return;
		const float scale = size / static_cast<float>(std::max(preview.Width, preview.Height));
		const ImVec2 extent(static_cast<float>(preview.Width) * scale, static_cast<float>(preview.Height) * scale);
		const ImVec2 min(origin.x + (size - extent.x) * 0.5f, origin.y + (size - extent.y) * 0.5f);
		drawList->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(preview.Texture.Get())), min, ImVec2(min.x + extent.x, min.y + extent.y));
	}

	void DrawPreviewSquare(const ThumbnailManager::Preview& preview, float size)
	{
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(size, size));
		DrawPreviewImage(ImGui::GetWindowDrawList(), preview, origin, size);
		if (preview.Status == ThumbnailManager::State::Failed && ImGui::BeginItemTooltip())
		{
			ImGui::Text("No preview: %s", preview.Error.c_str());
			ImGui::EndTooltip();
		}
	}

}
