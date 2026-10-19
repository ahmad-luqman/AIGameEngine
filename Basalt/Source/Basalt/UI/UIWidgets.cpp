#include "Basalt/UI/UIWidgets.h"

#include "Basalt/Core/Assert.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Basalt {

	const char* UIWidgetComponentName(UIWidgetKind kind)
	{
		switch (kind)
		{
			case UIWidgetKind::Button:
				return "UIButton";
			case UIWidgetKind::Slider:
				return "UISlider";
			case UIWidgetKind::Checkbox:
				return "UICheckbox";
			case UIWidgetKind::TextInput:
				return "UITextInput";
			case UIWidgetKind::Dropdown:
				return "UIDropdown";
			case UIWidgetKind::Panel:
				return "UIPanel";
			case UIWidgetKind::ScrollView:
				return "UIScrollView";
		}
		BS_CORE_ASSERT(false, "unknown widget kind {}", static_cast<int>(kind));
		return "UIButton";
	}

	std::optional<UIWidgetKind> UIWidgetKindFromComponentName(std::string_view name)
	{
		for (UIWidgetKind kind : { UIWidgetKind::Button, UIWidgetKind::Slider, UIWidgetKind::Checkbox, UIWidgetKind::TextInput, UIWidgetKind::Dropdown, UIWidgetKind::Panel, UIWidgetKind::ScrollView })
		{
			if (name == UIWidgetComponentName(kind))
				return kind;
		}
		return std::nullopt;
	}

	const char* UIWidgetStateName(UIWidgetState state)
	{
		switch (state)
		{
			case UIWidgetState::Normal:
				return "Normal";
			case UIWidgetState::Highlighted:
				return "Highlighted";
			case UIWidgetState::Pressed:
				return "Pressed";
			case UIWidgetState::Disabled:
				return "Disabled";
		}
		BS_CORE_ASSERT(false, "unknown widget state {}", static_cast<int>(state));
		return "Normal";
	}

	const glm::vec4& UIWidgetTints::For(UIWidgetState state) const
	{
		switch (state)
		{
			case UIWidgetState::Normal:
				return Normal;
			case UIWidgetState::Highlighted:
				return Highlight;
			case UIWidgetState::Pressed:
				return Pressed;
			case UIWidgetState::Disabled:
				return Disabled;
		}
		BS_CORE_ASSERT(false, "unknown widget state {}", static_cast<int>(state));
		return Normal;
	}

	namespace UIWidgets {

		std::vector<std::string_view> DropdownOptions(std::string_view options)
		{
			std::vector<std::string_view> result;
			if (options.empty())
				return result;
			while (result.size() < 128)
			{
				const size_t end = options.find('\n');
				result.push_back(options.substr(0, end));
				if (end == std::string_view::npos)
					break;
				options.remove_prefix(end + 1);
			}
			return result;
		}

		size_t NextCharacter(std::string_view text, size_t cursor)
		{
			if (cursor >= text.size())
				return text.size();
			cursor++;
			while (cursor < text.size() && (static_cast<unsigned char>(text[cursor]) & 0xc0U) == 0x80U)
				cursor++;
			return cursor;
		}

		size_t PreviousCharacter(std::string_view text, size_t cursor)
		{
			cursor = std::min(cursor, text.size());
			if (cursor == 0)
				return 0;
			cursor--;
			while (cursor > 0 && (static_cast<unsigned char>(text[cursor]) & 0xc0U) == 0x80U)
				cursor--;
			return cursor;
		}

		size_t TextLength(std::string_view text)
		{
			size_t count = 0;
			for (size_t cursor = 0; cursor < text.size(); cursor = NextCharacter(text, cursor))
				count++;
			return count;
		}

		bool IsSingleLineUTF8(std::string_view text)
		{
			for (size_t i = 0; i < text.size();)
			{
				const auto first = static_cast<unsigned char>(text[i++]);
				if (first < 0x80U)
				{
					if (first < 0x20U || first == 0x7fU)
						return false;
					continue;
				}
				const size_t extra = first >= 0xc2U && first <= 0xdfU ? 1 : first >= 0xe0U && first <= 0xefU ? 2
																		: first >= 0xf0U && first <= 0xf4U   ? 3
																											 : 0;
				if (extra == 0 || text.size() - i < extra)
					return false;
				uint32_t point = first & (0x7fU >> (extra + 1));
				for (size_t j = 0; j < extra; j++)
				{
					const auto next = static_cast<unsigned char>(text[i++]);
					if ((next & 0xc0U) != 0x80U)
						return false;
					point = (point << 6U) | (next & 0x3fU);
				}
				if ((extra == 1 && point < 0x80U) || (extra == 2 && point < 0x800U) || (extra == 3 && point < 0x10000U) || (point >= 0xd800U && point <= 0xdfffU) || point > 0x10ffffU)
					return false;
			}
			return true;
		}

		float SliderFraction(float value, float min, float max)
		{
			if (!(max > min))
				return 0.0f;
			return std::clamp((value - min) / (max - min), 0.0f, 1.0f);
		}

		float SnapSliderValue(float value, float min, float max, float step)
		{
			BS_CORE_ASSERT(min < max, "a slider range must not be empty");
			value = std::clamp(value, min, max);
			if (!(step > 0.0f))
				return value;
			const float steps = std::round((value - min) / step);
			// A step tiny against the range overflows the count: no snapping is visible at that scale.
			if (!std::isfinite(steps))
				return value;
			const float snapped = std::clamp(min + steps * step, min, max);
			// Max is a stop too, even when the range is not a whole number of steps.
			return std::abs(max - value) < std::abs(snapped - value) ? max : snapped;
		}

		float SliderValueAtFraction(float fraction, float min, float max, float step)
		{
			return SnapSliderValue(min + std::clamp(fraction, 0.0f, 1.0f) * (max - min), min, max, step);
		}

		float StepSliderValue(float value, int steps, float min, float max, float step)
		{
			BS_CORE_ASSERT(min < max, "a slider range must not be empty");
			if (!(step > 0.0f))
				return std::clamp(value + static_cast<float>(steps) * (max - min) / ContinuousNavigationStops, min, max);
			// Count stops in double: Min + k * Step for k = 0..last grid stop, then Max when it is past the grid.
			const double first = min;
			const double last = max;
			const double stride = step;
			const double grid = std::floor((last - first) / stride);
			// More stops than a float can tell apart: step by value and snap.
			constexpr double MaxStops = 1e7;
			if (!(grid <= MaxStops))
				return SnapSliderValue(value + static_cast<float>(steps) * step, min, max, step);
			const auto lastGrid = static_cast<int64_t>(grid);
			const bool maxIsExtraStop = first + grid * stride < last;
			const int64_t lastStop = lastGrid + (maxIsExtraStop ? 1 : 0);
			// The stop nearest the value (Max included).
			int64_t current = std::clamp<int64_t>(std::llround((static_cast<double>(value) - first) / stride), 0, lastGrid);
			if (maxIsExtraStop && last - value < std::abs(value - (first + static_cast<double>(current) * stride)))
				current = lastStop;
			const int64_t target = std::clamp<int64_t>(current + steps, 0, lastStop);
			if (target == lastStop)
				return max;
			return std::min(static_cast<float>(first + static_cast<double>(target) * stride), max);
		}

		void AppendRect(const UIBox& box, const glm::vec4& color, std::vector<UIDrawCommand>& out)
		{
			UIDrawCommand& command = out.emplace_back();
			command.Kind = UIDrawKind::Rect;
			command.Position = box.Min;
			command.Size = box.Size;
			command.Color = color;
		}

		float SliderHandleWidth(const UIBox& rect, float handleWidth)
		{
			return std::clamp(handleWidth, 0.0f, rect.Size.x);
		}

		UIBox SliderHandleBox(const UIBox& rect, float fraction, float handleWidth)
		{
			const float width = SliderHandleWidth(rect, handleWidth);
			const float travel = rect.Size.x - width;
			return { { rect.Min.x + travel * std::clamp(fraction, 0.0f, 1.0f), rect.Min.y }, { width, rect.Size.y } };
		}

		float SliderFractionAt(const UIBox& rect, float handleWidth, float x)
		{
			const float width = SliderHandleWidth(rect, handleWidth);
			const float travel = rect.Size.x - width;
			// A handle as wide as the rect has nowhere to go: the pointer's side of the center decides.
			if (travel <= 0.0f)
				return x < rect.Center().x ? 0.0f : 1.0f;
			return std::clamp((x - rect.Min.x - width * 0.5f) / travel, 0.0f, 1.0f);
		}

		UIBox CheckboxBox(const UIBox& rect)
		{
			const float side = std::min(rect.Size.x, rect.Size.y);
			return { { rect.Min.x, rect.Min.y + (rect.Size.y - side) * 0.5f }, { side, side } };
		}

		UIBox CheckboxMark(const UIBox& box)
		{
			const glm::vec2 inset = box.Size * 0.25f;
			return { box.Min + inset, box.Size - inset * 2.0f };
		}

		UIBox CheckboxLabelBox(const UIBox& rect)
		{
			const float offset = std::min(CheckboxBox(rect).Size.x * 1.25f, rect.Size.x);
			return { { rect.Min.x + offset, rect.Min.y }, { rect.Size.x - offset, rect.Size.y } };
		}

		void AppendSlider(const UIBox& rect, float fraction, float handleWidth, const glm::vec4& trackColor, const glm::vec4& fillColor, const glm::vec4& handleColor, std::vector<UIDrawCommand>& out, bool vertical)
		{
			if (vertical)
			{
				const UIBox swapped{ { rect.Min.y, rect.Min.x }, { rect.Size.y, rect.Size.x } };
				const size_t start = out.size();
				AppendSlider(swapped, 1.0f - fraction, handleWidth, trackColor, fillColor, handleColor, out);
				// Vertical values grow upwards, including the fill from the bottom edge.
				const UIBox handle = SliderHandleBox(swapped, 1.0f - fraction, handleWidth);
				out[start + 1].Position.x = handle.Center().x;
				out[start + 1].Size.x = swapped.Min.x + swapped.Size.x - handle.Center().x;
				for (size_t i = start; i < out.size(); i++)
				{
					std::swap(out[i].Position.x, out[i].Position.y);
					std::swap(out[i].Size.x, out[i].Size.y);
				}
				return;
			}
			const UIBox handle = SliderHandleBox(rect, fraction, handleWidth);
			AppendRect(rect, trackColor, out);
			// The fill reaches the handle's center.
			AppendRect({ rect.Min, { handle.Center().x - rect.Min.x, rect.Size.y } }, fillColor, out);
			AppendRect(handle, handleColor, out);
		}

		void AppendCheckbox(const UIBox& rect, bool checked, const glm::vec4& boxColor, const glm::vec4& checkColor, std::vector<UIDrawCommand>& out)
		{
			const UIBox box = CheckboxBox(rect);
			AppendRect(box, boxColor, out);
			if (checked)
				AppendRect(CheckboxMark(box), checkColor, out);
		}

	}

}
