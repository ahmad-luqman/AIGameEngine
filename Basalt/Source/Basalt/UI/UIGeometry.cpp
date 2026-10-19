#include "Basalt/UI/UIGeometry.h"

#include "Basalt/UI/GameUI.h"
#include "Basalt/UI/UIRules.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

	glm::mat3 ComputeUITransform(const UIBox& box, const UIRectComponent& rect)
	{
		const glm::vec2 pivot = box.Min + box.Size * rect.Pivot;
		const float c = std::cos(rect.Rotation), s = std::sin(rect.Rotation);
		glm::mat3 result(1.0f);
		result[0] = { c * rect.Scale.x, s * rect.Scale.x, 0.0f };
		result[1] = { -s * rect.Scale.y, c * rect.Scale.y, 0.0f };
		result[2] = glm::vec3(pivot - glm::vec2(result * glm::vec3(pivot, 0.0f)), 1.0f);
		return result;
	}

	UIBox TransformUIBox(const UIBox& box, const glm::mat3& transform)
	{
		glm::vec2 low(std::numeric_limits<float>::max()), high(std::numeric_limits<float>::lowest());
		for (glm::vec2 p : { box.Min, box.Max(), box.Min + glm::vec2(box.Size.x, 0), box.Min + glm::vec2(0, box.Size.y) })
		{
			p = glm::vec2(transform * glm::vec3(p, 1));
			low = glm::min(low, p);
			high = glm::max(high, p);
		}
		return { low, high - low };
	}

	UIBox IntersectUIBoxes(const UIBox& a, const UIBox& b)
	{
		const glm::vec2 low = glm::max(a.Min, b.Min);
		return { low, glm::max(glm::min(a.Max(), b.Max()) - low, glm::vec2(0)) };
	}

	bool IsUsableUITransform(const UIBox& box, const glm::mat3& transform)
	{
		if (!std::isfinite(box.Min.x) || !std::isfinite(box.Min.y) || !std::isfinite(box.Size.x) || !std::isfinite(box.Size.y) || box.Size.x < 0 || box.Size.y < 0)
			return false;
		for (glm::length_t col = 0; col < 3; ++col)
			for (glm::length_t row = 0; row < 3; ++row)
				if (!std::isfinite(transform[col][row]) || std::abs(transform[col][row]) > MaxUICoordinate)
					return false;
		if (transform[0][2] != 0 || transform[1][2] != 0 || transform[2][2] != 1)
			return false;
		const double determinant = double(transform[0][0]) * transform[1][1] - double(transform[1][0]) * transform[0][1];
		if (determinant == 0 || !std::isfinite(determinant))
			return false;
		for (glm::length_t col = 0; col < 2; ++col)
			for (glm::length_t row = 0; row < 2; ++row)
				if (std::abs(double(transform[col][row]) / determinant) > 1e12)
					return false;
		const UIBox bounds = TransformUIBox(box, transform);
		for (glm::vec2 point : { bounds.Min, bounds.Max() })
			if (!std::isfinite(point.x) || !std::isfinite(point.y) || std::abs(point.x) > MaxUICoordinate || std::abs(point.y) > MaxUICoordinate)
				return false;
		return true;
	}

	bool ContainsUIBox(const UIBox& box, const glm::mat3& transform, const glm::vec2& point)
	{
		if (!IsUsableUITransform(box, transform))
			return false;
		// Double intermediates preserve valid tiny scales whose float determinant would underflow.
		const double determinant = double(transform[0][0]) * transform[1][1] - double(transform[1][0]) * transform[0][1];
		const double x = double(point.x) - transform[2][0], y = double(point.y) - transform[2][1];
		const glm::dvec2 local((x * transform[1][1] - y * transform[1][0]) / determinant,
							   (y * transform[0][0] - x * transform[0][1]) / determinant);
		return local.x >= box.Min.x && local.y >= box.Min.y && local.x < double(box.Min.x) + box.Size.x && local.y < double(box.Min.y) + box.Size.y;
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
