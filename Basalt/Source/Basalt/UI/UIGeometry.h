#pragma once

#include "Basalt/Scene/Components.h"

#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace Basalt {

	// An axis-aligned rectangle on the UI canvas (canvas units, origin top-left, +Y down). Every box the UI
	// produces has a non-negative size.
	struct UIBox
	{
		glm::vec2 Min = { 0.0f, 0.0f };
		glm::vec2 Size = { 0.0f, 0.0f };

		glm::vec2 Max() const { return Min + Size; }
		glm::vec2 Center() const { return Min + Size * 0.5f; }
		// Min inclusive, Max exclusive, so adjacent boxes never both contain a point.
		bool Contains(const glm::vec2& point) const { return point.x >= Min.x && point.y >= Min.y && point.x < Min.x + Size.x && point.y < Min.y + Size.y; }
		bool operator==(const UIBox&) const = default;
	};

	// Maps the canvas onto a screen rectangle: the canvas is GameUI::CanvasHeight units tall, so one unit is
	// `Scale` screen (ImGui) units (the same on both axes). Shared by the UI renderer and the editor's UI overlay.
	struct UICanvasMapping
	{
		glm::vec2 Origin = { 0.0f, 0.0f };
		float Scale = 1.0f;

		// The mapping that shows the whole canvas on the screen rectangle [origin, origin + size); size.y > 0.
		static UICanvasMapping ForViewport(const glm::vec2& origin, const glm::vec2& size);

		glm::vec2 ToScreen(const glm::vec2& canvasPoint) const { return Origin + canvasPoint * Scale; }
		UIBox ToScreen(const UIBox& box) const { return { ToScreen(box.Min), box.Size * Scale }; }
		// The inverse of ToScreen for points (Scale > 0).
		glm::vec2 ToCanvas(const glm::vec2& screenPoint) const { return (screenPoint - Origin) / Scale; }
	};

	// The rect a UIRectComponent describes inside its parent's rect (see UIRectComponent). A negative
	// resulting size (Size shrinking the anchors' span past zero) collapses to zero around the pivot.
	UIBox ComputeUIRect(const UIBox& parent, const UIRectComponent& rect);

	// Composed transforms must remain finite, with coordinates/linear terms within 1e9 and inverse linear terms within 1e12.
	bool IsUsableUITransform(const UIBox& box, const glm::mat3& transform);
	constexpr float MaxUICoordinate = 1e9f;
	glm::mat3 ComputeUITransform(const UIBox& box, const UIRectComponent& rect);
	UIBox TransformUIBox(const UIBox& box, const glm::mat3& transform);
	UIBox IntersectUIBoxes(const UIBox& a, const UIBox& b);
	bool ContainsUIBox(const UIBox& box, const glm::mat3& transform, const glm::vec2& point);

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
	// units. Opposite borders that together exceed the texture shrink proportionally to fit it, and so do
	// borders that would overlap in `box`, so the corners never invert. Patches can have zero size (no border
	// on that side).
	std::array<UIQuad, 9> ComputeNineSlice(const UIBox& box, const glm::vec2& textureSize, const glm::vec4& slice, float sliceScale);

	// Named canvas anchors for immediate-mode UI ("TopLeft", "Center", "BottomRight", ...), as fractions of
	// the canvas. nullopt for an unknown name.
	std::optional<glm::vec2> FindUIAnchorPreset(std::string_view name);
	std::span<const std::string_view> GetUIAnchorPresetNames();

	// The anchor preset that stretches a rect over its parent; the others are FindUIAnchorPreset's points.
	constexpr std::string_view UIStretchPreset = "Stretch";

	// The editor's anchor presets: a named point (see FindUIAnchorPreset) or UIStretchPreset. Returns `rect`
	// with its anchors and pivot set to the preset and its Position and Size recomputed so that, inside
	// `parent`, it covers the same box as before: only how it follows its parent's size changes (unless
	// Position or Size would leave their range, which clamps them). nullopt for an unknown preset.
	std::optional<UIRectComponent> ApplyUIAnchorPreset(const UIRectComponent& rect, std::string_view preset, const UIBox& parent);

}
