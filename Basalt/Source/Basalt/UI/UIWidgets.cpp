#include "Basalt/UI/UIWidgets.h"

#include <algorithm>
#include <cmath>

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
		}
		return "UIButton";
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
		return Normal;
	}

	namespace UIWidgets {

		namespace {

			void AppendRect(const UIBox& box, const glm::vec4& color, std::vector<UIDrawCommand>& out)
			{
				UIDrawCommand& command = out.emplace_back();
				command.Kind = UIDrawKind::Rect;
				command.Position = box.Min;
				command.Size = box.Size;
				command.Color = color;
			}

		}

		float SliderFraction(float value, float min, float max)
		{
			if (!(max > min))
				return 0.0f;
			return std::clamp((value - min) / (max - min), 0.0f, 1.0f);
		}

		float SnapSliderValue(float value, float min, float max, float step)
		{
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

		float SliderNavigationStep(float min, float max, float step)
		{
			return step > 0.0f ? step : (max - min) / 20.0f;
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

		void AppendSlider(const UIBox& rect, float fraction, float handleWidth, const glm::vec4& trackColor, const glm::vec4& fillColor, const glm::vec4& handleColor, std::vector<UIDrawCommand>& out)
		{
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
