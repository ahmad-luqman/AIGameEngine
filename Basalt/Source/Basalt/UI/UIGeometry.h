#pragma once

#include "Basalt/Scene/Components.h"

#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace Basalt {

	// An axis-aligned rectangle on the UI canvas (canvas units, origin top-left, +Y down).
	struct UIBox
	{
		glm::vec2 Min = { 0.0f, 0.0f };
		glm::vec2 Size = { 0.0f, 0.0f };

		glm::vec2 Max() const { return Min + Size; }
		bool Contains(const glm::vec2& point) const { return point.x >= Min.x && point.y >= Min.y && point.x < Min.x + Size.x && point.y < Min.y + Size.y; }
		bool operator==(const UIBox&) const = default;
	};

	// The rect a UIRectComponent describes inside its parent's rect (see UIRectComponent). A negative
	// resulting size (Size shrinking the anchors' span past zero) collapses to zero around the pivot.
	UIBox ComputeUIRect(const UIBox& parent, const UIRectComponent& rect);

	// The top-left corner that places something of size `extent` inside `box` at `align` (0 = left/top,
	// 0.5 = centered, 1 = right/bottom). A zero-size box makes `align` the extent's pivot on box.Min.
	glm::vec2 AlignInBox(const UIBox& box, const glm::vec2& extent, const glm::vec2& align);

	// One textured quad of a nine-slice panel.
	struct UIQuad
	{
		UIBox Box;
		glm::vec2 UVMin = { 0.0f, 0.0f };
		glm::vec2 UVMax = { 1.0f, 1.0f };
	};

	// Splits `box` into the nine patches of a nine-slice image (row by row, top-left first). `slice` holds
	// the borders in texels (left, top, right, bottom); on the canvas each border is `slice * sliceScale`
	// units. Borders wider than the texture are clamped to it, and borders that would overlap in `box` shrink
	// proportionally, so the corners never invert. Patches can have zero size (no border on that side).
	std::array<UIQuad, 9> ComputeNineSlice(const UIBox& box, const glm::vec2& textureSize, const glm::vec4& slice, float sliceScale);

	// Named canvas anchors for immediate-mode UI ("TopLeft", "Center", "BottomRight", ...), as fractions of
	// the canvas. nullopt for an unknown name.
	std::optional<glm::vec2> FindUIAnchorPreset(std::string_view name);
	std::span<const std::string_view> GetUIAnchorPresetNames();

}
