#include "Basalt/UI/UIGeometry.h"

#include "Basalt/UI/GameUI.h"
#include "Basalt/UI/UIRules.h"

#include <algorithm>

namespace Basalt {

	namespace {

		struct AnchorPreset
		{
			std::string_view Name;
			glm::vec2 Anchor;
		};

		constexpr AnchorPreset AnchorPresets[] = {
			{ "TopLeft", { 0.0f, 0.0f } },
			{ "Top", { 0.5f, 0.0f } },
			{ "TopRight", { 1.0f, 0.0f } },
			{ "Left", { 0.0f, 0.5f } },
			{ "Center", { 0.5f, 0.5f } },
			{ "Right", { 1.0f, 0.5f } },
			{ "BottomLeft", { 0.0f, 1.0f } },
			{ "Bottom", { 0.5f, 1.0f } },
			{ "BottomRight", { 1.0f, 1.0f } },
		};

		constexpr std::array<std::string_view, std::size(AnchorPresets)> AnchorPresetNames = [] {
			std::array<std::string_view, std::size(AnchorPresets)> names{};
			for (size_t i = 0; i < names.size(); i++)
				names[i] = AnchorPresets[i].Name;
			return names;
		}();

		// Scales `a` and `b` down together so their sum is at most `limit`.
		void ShrinkToFit(float& a, float& b, float limit)
		{
			const float total = a + b;
			if (total > limit && total > 0.0f)
			{
				const float shrink = std::max(limit, 0.0f) / total;
				a *= shrink;
				b *= shrink;
			}
		}

		// Border sizes along one axis, in texels and on the canvas.
		struct AxisSlice
		{
			float TexelBefore;
			float TexelAfter;
			float CanvasBefore;
			float CanvasAfter;
		};

		AxisSlice SliceAxis(float before, float after, float textureLength, float length, float scale)
		{
			AxisSlice axis{ std::max(before, 0.0f), std::max(after, 0.0f), 0.0f, 0.0f };
			ShrinkToFit(axis.TexelBefore, axis.TexelAfter, textureLength);
			axis.CanvasBefore = axis.TexelBefore * scale;
			axis.CanvasAfter = axis.TexelAfter * scale;
			ShrinkToFit(axis.CanvasBefore, axis.CanvasAfter, length);
			return axis;
		}

	}

	UICanvasMapping UICanvasMapping::ForViewport(const glm::vec2& origin, const glm::vec2& size)
	{
		return { origin, size.y / GameUI::CanvasHeight };
	}

	UIBox ComputeUIRect(const UIBox& parent, const UIRectComponent& rect)
	{
		const glm::vec2 anchorMin = parent.Min + parent.Size * rect.AnchorMin;
		const glm::vec2 anchorMax = parent.Min + parent.Size * rect.AnchorMax;
		const glm::vec2 span = anchorMax - anchorMin;
		const glm::vec2 pivot = anchorMin + span * rect.Pivot + rect.Position;
		const glm::vec2 size = glm::max(span + rect.Size, glm::vec2(0.0f));
		return { pivot - size * rect.Pivot, size };
	}

	glm::vec2 AlignInBox(const UIBox& box, const glm::vec2& extent, const glm::vec2& align)
	{
		return box.Min + (box.Size - extent) * align;
	}

	std::array<UIQuad, 9> ComputeNineSlice(const UIBox& box, const glm::vec2& textureSize, const glm::vec4& slice, float sliceScale)
	{
		const AxisSlice x = SliceAxis(slice.x, slice.z, textureSize.x, box.Size.x, sliceScale);
		const AxisSlice y = SliceAxis(slice.y, slice.w, textureSize.y, box.Size.y, sliceScale);

		// Patch edges on the canvas and in texture space, left/top to right/bottom.
		const glm::vec2 max = box.Max();
		const float canvasX[4] = { box.Min.x, box.Min.x + x.CanvasBefore, max.x - x.CanvasAfter, max.x };
		const float canvasY[4] = { box.Min.y, box.Min.y + y.CanvasBefore, max.y - y.CanvasAfter, max.y };
		const auto uv = [](float texels, float length) { return length > 0.0f ? texels / length : 0.0f; };
		const float u[4] = { 0.0f, uv(x.TexelBefore, textureSize.x), 1.0f - uv(x.TexelAfter, textureSize.x), 1.0f };
		const float v[4] = { 0.0f, uv(y.TexelBefore, textureSize.y), 1.0f - uv(y.TexelAfter, textureSize.y), 1.0f };

		std::array<UIQuad, 9> quads{};
		for (size_t row = 0; row < 3; row++)
		{
			for (size_t column = 0; column < 3; column++)
			{
				UIQuad& quad = quads[row * 3 + column];
				quad.Box.Min = { canvasX[column], canvasY[row] };
				quad.Box.Size = { canvasX[column + 1] - canvasX[column], canvasY[row + 1] - canvasY[row] };
				quad.UVMin = { u[column], v[row] };
				quad.UVMax = { u[column + 1], v[row + 1] };
			}
		}
		return quads;
	}

	std::optional<glm::vec2> FindUIAnchorPreset(std::string_view name)
	{
		for (const AnchorPreset& preset : AnchorPresets)
		{
			if (preset.Name == name)
				return preset.Anchor;
		}
		return std::nullopt;
	}

	std::span<const std::string_view> GetUIAnchorPresetNames()
	{
		return AnchorPresetNames;
	}

	std::optional<UIRectComponent> ApplyUIAnchorPreset(const UIRectComponent& rect, std::string_view preset, const UIBox& parent)
	{
		const UIBox current = ComputeUIRect(parent, rect);
		UIRectComponent result = rect;
		if (preset == UIStretchPreset)
		{
			result.AnchorMin = { 0.0f, 0.0f };
			result.AnchorMax = { 1.0f, 1.0f };
			result.Pivot = { 0.5f, 0.5f };
		}
		else if (std::optional<glm::vec2> point = FindUIAnchorPreset(preset))
		{
			result.AnchorMin = result.AnchorMax = result.Pivot = *point;
		}
		else
		{
			return std::nullopt;
		}
		// Inverse of ComputeUIRect: Size is what the box adds to the anchors' span, Position how far the box's
		// pivot is from the anchors' pivot point.
		const glm::vec2 anchorMin = parent.Min + parent.Size * result.AnchorMin;
		const glm::vec2 span = parent.Size * (result.AnchorMax - result.AnchorMin);
		result.Size = current.Size - span;
		result.Position = (current.Min + current.Size * result.Pivot) - (anchorMin + span * result.Pivot);
		return SanitizeUIRect(result);
	}

}
