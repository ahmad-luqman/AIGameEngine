#pragma once

#include "Basalt/UI/UIDrawCommand.h"
#include "Basalt/UI/UIGeometry.h"

#include <glm/glm.hpp>

#include <vector>

namespace Basalt {

	enum class UIWidgetKind
	{
		Button = 0,
		Slider,
		Checkbox
	};

	// The component name of a widget kind ("UIButton", "UISlider", "UICheckbox").
	const char* UIWidgetComponentName(UIWidgetKind kind);

	// How a widget looks this frame; picks one of its tints.
	enum class UIWidgetState
	{
		Normal = 0,
		// Hovered by the pointer, or focused by keyboard/gamepad navigation.
		Highlighted,
		// Held down by the pointer or the accept button.
		Pressed,
		// Not Interactable.
		Disabled
	};

	// "Normal", "Highlighted", "Pressed" or "Disabled".
	const char* UIWidgetStateName(UIWidgetState state);

	// The four tints of a widget, picked by state.
	struct UIWidgetTints
	{
		glm::vec4 Normal = { 1.0f, 1.0f, 1.0f, 1.0f };
		glm::vec4 Highlight = { 1.0f, 1.0f, 1.0f, 1.0f };
		glm::vec4 Pressed = { 1.0f, 1.0f, 1.0f, 1.0f };
		glm::vec4 Disabled = { 1.0f, 1.0f, 1.0f, 1.0f };

		template<typename T>
		static UIWidgetTints Of(const T& widget)
		{
			return { widget.NormalTint, widget.HighlightTint, widget.PressedTint, widget.DisabledTint };
		}

		const glm::vec4& For(UIWidgetState state) const;
	};

	// Geometry and values shared by the widget drawing (UILayout, the immediate-mode UI.* widgets) and the hit
	// testing (UIInteraction), so what is drawn is what responds.
	namespace UIWidgets {

		// Where Value sits between Min and Max, 0..1 (Max > Min).
		float SliderFraction(float value, float min, float max);
		// Value clamped to [min, max] and, with a positive step, moved to the nearest Min + k * step.
		float SnapSliderValue(float value, float min, float max, float step);
		// How far one navigation press moves a slider: Step, or a twentieth of the range when continuous.
		float SliderNavigationStep(float min, float max, float step);

		// The handle's width in `rect`: HandleWidth, at most the rect's width.
		float SliderHandleWidth(const UIBox& rect, float handleWidth);
		// The handle's box at `fraction`: as tall as the rect, its center travelling from half a handle inside
		// the left edge to half a handle inside the right edge, so it never leaves the rect.
		UIBox SliderHandleBox(const UIBox& rect, float fraction, float handleWidth);
		// The fraction whose handle center is at canvas x (clamped to 0..1).
		float SliderFractionAt(const UIBox& rect, float handleWidth, float x);

		// The checkbox's box: a square as tall as the rect (at most as wide) at its left edge, centered vertically.
		UIBox CheckboxBox(const UIBox& rect);
		// The check mark inside the box.
		UIBox CheckboxMark(const UIBox& box);
		// Where the label goes: the rest of the rect, right of the box after a gap of a quarter box.
		UIBox CheckboxLabelBox(const UIBox& rect);

		// Appends the slider's track, fill and handle as Rect commands (Anchor and Pivot zero; Position = box
		// top-left). `handleColor` is already tinted.
		void AppendSlider(const UIBox& rect, float fraction, float handleWidth, const glm::vec4& trackColor, const glm::vec4& fillColor, const glm::vec4& handleColor, std::vector<UIDrawCommand>& out);
		// Appends the checkbox's box and, when checked, its mark as Rect commands. `boxColor` is already tinted.
		void AppendCheckbox(const UIBox& rect, bool checked, const glm::vec4& boxColor, const glm::vec4& checkColor, std::vector<UIDrawCommand>& out);

	}

}
