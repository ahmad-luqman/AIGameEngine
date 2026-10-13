#include "Basalt/UI/UIGeometry.h"

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

		// Border sizes along one axis: texel borders clamped to the texture, canvas borders shrunk together
		// when they would overlap inside `length`.
		struct AxisSlice
		{
			float TexelBefore;
			float TexelAfter;
			float CanvasBefore;
			float CanvasAfter;
		};

		AxisSlice SliceAxis(float before, float after, float textureLength, float length, float scale)
		{
			AxisSlice axis{};
			before = std::max(before, 0.0f);
			after = std::max(after, 0.0f);
			if (before + after > textureLength && before + after > 0.0f)
			{
				const float shrink = textureLength / (before + after);
				before *= shrink;
				after *= shrink;
			}
			axis.TexelBefore = before;
			axis.TexelAfter = after;
			axis.CanvasBefore = before * scale;
			axis.CanvasAfter = after * scale;
			const float total = axis.CanvasBefore + axis.CanvasAfter;
			if (total > length && total > 0.0f)
			{
				const float shrink = std::max(length, 0.0f) / total;
				axis.CanvasBefore *= shrink;
				axis.CanvasAfter *= shrink;
			}
			return axis;
		}

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
		const float canvasX[4] = { box.Min.x, box.Min.x + x.CanvasBefore, box.Min.x + box.Size.x - x.CanvasAfter, box.Min.x + box.Size.x };
		const float canvasY[4] = { box.Min.y, box.Min.y + y.CanvasBefore, box.Min.y + box.Size.y - y.CanvasAfter, box.Min.y + box.Size.y };
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

}
