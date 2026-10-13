#include "Basalt/UI/UIRules.h"

#include "Basalt/Core/Log.h"

#include <algorithm>
#include <cmath>

namespace Basalt {

	namespace {

		constexpr const char* AxisNames[] = { ".x", ".y", ".z", ".w" };
		constexpr const char* ChannelNames[] = { ".r", ".g", ".b", ".a" };

		// Walks a component's rules once. A field whose value differs from the value it must have (clamped,
		// or the default when it is not finite) breaks its rule: the first such field is reported and, when
		// repairing, every one is replaced. NaN differs from everything, -0.0 equals 0.0.
		class RuleWalker
		{
		public:
			explicit RuleWalker(bool repair)
				: m_Repair(repair)
			{
			}

			void Float(float& value, float min, float max, float fallback, const std::string& name)
			{
				const float required = ClampOrFallback(value, min, max, fallback);
				if (required == value)
					return;
				Report(fmt::format("{} must be between {} and {} (got {})", name, min, max, value));
				if (m_Repair)
					value = required;
			}

			template<glm::length_t L>
			void Vector(glm::vec<L, float, glm::defaultp>& value, float min, float max, const glm::vec<L, float, glm::defaultp>& fallback, const char* name, const char* const* componentNames)
			{
				for (glm::length_t i = 0; i < L; i++)
					Float(value[i], min, max, fallback[i], std::string(name) + componentNames[i]);
			}

			// Display colors and opacity, 0..1.
			void Color(glm::vec4& value, const glm::vec4& fallback, const char* name)
			{
				Vector(value, 0.0f, 1.0f, fallback, name, ChannelNames);
			}

			template<typename E>
			void Enum(E& value, size_t count, E fallback, const char* name)
			{
				if (static_cast<size_t>(value) < count)
					return;
				Report(fmt::format("{} must be one of its {} values (got {})", name, count, static_cast<size_t>(value)));
				if (m_Repair)
					value = fallback;
			}

			// A widget callback name: a Lua identifier or empty.
			void CallbackName(std::string& value, const char* name)
			{
				if (value.empty() || IsValidUICallbackName(value))
					return;
				Report(fmt::format("{} must be the name of a script method (a Lua identifier of at most {} characters) or empty (got '{}')", name, MaxUICallbackNameLength, value));
				if (m_Repair)
					value.clear();
			}

			void Report(std::string problem)
			{
				if (m_Problem.empty())
					m_Problem = std::move(problem);
			}

			bool Repairing() const { return m_Repair; }
			std::string TakeProblem() { return std::move(m_Problem); }

		private:
			bool m_Repair = false;
			std::string m_Problem;
		};

		void ApplyRules(UIRectComponent& rect, RuleWalker& rules)
		{
			const UIRectComponent defaults;
			constexpr float Limit = UIRectComponent::MaxMagnitude;
			rules.Vector(rect.AnchorMin, 0.0f, 1.0f, defaults.AnchorMin, "AnchorMin", AxisNames);
			rules.Vector(rect.AnchorMax, 0.0f, 1.0f, defaults.AnchorMax, "AnchorMax", AxisNames);
			for (glm::length_t axis = 0; axis < 2; axis++)
			{
				// After the range rules: when repairing, both anchors are finite here; when validating, a NaN
				// anchor was already reported (only the first problem is kept).
				if (rect.AnchorMax[axis] >= rect.AnchorMin[axis])
					continue;
				rules.Report(fmt::format("AnchorMax{} must not be less than AnchorMin{} (got {} < {})", AxisNames[axis], AxisNames[axis], rect.AnchorMax[axis], rect.AnchorMin[axis]));
				if (rules.Repairing())
					rect.AnchorMax[axis] = rect.AnchorMin[axis];
			}
			rules.Vector(rect.Pivot, 0.0f, 1.0f, defaults.Pivot, "Pivot", AxisNames);
			rules.Vector(rect.Position, -Limit, Limit, defaults.Position, "Position", AxisNames);
			rules.Vector(rect.Size, -Limit, Limit, defaults.Size, "Size", AxisNames);
		}

		void ApplyRules(UIImageComponent& image, RuleWalker& rules)
		{
			const UIImageComponent defaults;
			rules.Color(image.Color, defaults.Color, "Color");
			rules.Vector(image.Slice, 0.0f, UIImageComponent::MaxSlice, defaults.Slice, "Slice", AxisNames);
			rules.Float(image.SliceScale, UIImageComponent::MinSliceScale, UIImageComponent::MaxSliceScale, defaults.SliceScale, "SliceScale");
		}

		void ApplyRules(UITextComponent& text, RuleWalker& rules)
		{
			const UITextComponent defaults;
			rules.Float(text.FontSize, UITextComponent::MinFontSize, UITextComponent::MaxFontSize, defaults.FontSize, "FontSize");
			rules.Color(text.Color, defaults.Color, "Color");
			// Only C++ code can write these out of range; files and Lua go through the enum names.
			rules.Enum(text.HorizontalAlign, UIHorizontalAlignCount, defaults.HorizontalAlign, "HorizontalAlign");
			rules.Enum(text.VerticalAlign, UIVerticalAlignCount, defaults.VerticalAlign, "VerticalAlign");
		}

		// The tints and callback every widget has.
		template<typename T>
		void ApplyWidgetRules(T& widget, RuleWalker& rules, std::string& callback, const char* callbackName)
		{
			const T defaults;
			rules.Color(widget.NormalTint, defaults.NormalTint, "NormalTint");
			rules.Color(widget.HighlightTint, defaults.HighlightTint, "HighlightTint");
			rules.Color(widget.PressedTint, defaults.PressedTint, "PressedTint");
			rules.Color(widget.DisabledTint, defaults.DisabledTint, "DisabledTint");
			rules.CallbackName(callback, callbackName);
		}

		void ApplyRules(UIButtonComponent& button, RuleWalker& rules)
		{
			ApplyWidgetRules(button, rules, button.OnClick, "OnClick");
		}

		void ApplyRules(UISliderComponent& slider, RuleWalker& rules)
		{
			const UISliderComponent defaults;
			constexpr float Limit = UIRectComponent::MaxMagnitude;
			rules.Float(slider.Min, -Limit, Limit, defaults.Min, "Min");
			rules.Float(slider.Max, -Limit, Limit, defaults.Max, "Max");
			// After the range rules, as for the anchors: an empty range has no fraction for the value.
			if (!(slider.Max > slider.Min))
			{
				rules.Report(fmt::format("Max must be greater than Min (got Max {} and Min {})", slider.Max, slider.Min));
				if (rules.Repairing())
				{
					// A unit range above Min, or the top unit of the allowed range when Min is at its top.
					slider.Min = std::min(slider.Min, Limit - 1.0f);
					slider.Max = slider.Min + 1.0f;
				}
			}
			// When validating an empty or NaN range (already reported) there is no range to check Value against.
			if (slider.Max > slider.Min)
				rules.Float(slider.Value, slider.Min, slider.Max, slider.Min, "Value");
			rules.Float(slider.Step, 0.0f, Limit, defaults.Step, "Step");
			rules.Color(slider.TrackColor, defaults.TrackColor, "TrackColor");
			rules.Color(slider.FillColor, defaults.FillColor, "FillColor");
			rules.Color(slider.HandleColor, defaults.HandleColor, "HandleColor");
			rules.Float(slider.HandleWidth, 0.0f, Limit, defaults.HandleWidth, "HandleWidth");
			ApplyWidgetRules(slider, rules, slider.OnValueChanged, "OnValueChanged");
		}

		void ApplyRules(UICheckboxComponent& checkbox, RuleWalker& rules)
		{
			const UICheckboxComponent defaults;
			rules.Color(checkbox.BoxColor, defaults.BoxColor, "BoxColor");
			rules.Color(checkbox.CheckColor, defaults.CheckColor, "CheckColor");
			ApplyWidgetRules(checkbox, rules, checkbox.OnValueChanged, "OnValueChanged");
		}

		template<typename T>
		std::string Validate(T component)
		{
			RuleWalker rules(false);
			ApplyRules(component, rules);
			return rules.TakeProblem();
		}

		template<typename T>
		T Sanitize(T component)
		{
			RuleWalker rules(true);
			ApplyRules(component, rules);
			return component;
		}

	}

	std::string ValidateUIRect(const UIRectComponent& rect)
	{
		return Validate(rect);
	}

	std::string ValidateUIImage(const UIImageComponent& image)
	{
		return Validate(image);
	}

	std::string ValidateUIText(const UITextComponent& text)
	{
		return Validate(text);
	}

	UIRectComponent SanitizeUIRect(UIRectComponent rect)
	{
		return Sanitize(rect);
	}

	UIImageComponent SanitizeUIImage(UIImageComponent image)
	{
		return Sanitize(std::move(image));
	}

	UITextComponent SanitizeUIText(UITextComponent text)
	{
		return Sanitize(std::move(text));
	}

	std::string ValidateUIButton(const UIButtonComponent& button)
	{
		return Validate(button);
	}

	std::string ValidateUISlider(const UISliderComponent& slider)
	{
		return Validate(slider);
	}

	std::string ValidateUICheckbox(const UICheckboxComponent& checkbox)
	{
		return Validate(checkbox);
	}

	UIButtonComponent SanitizeUIButton(UIButtonComponent button)
	{
		return Sanitize(std::move(button));
	}

	UISliderComponent SanitizeUISlider(UISliderComponent slider)
	{
		return Sanitize(std::move(slider));
	}

	UICheckboxComponent SanitizeUICheckbox(UICheckboxComponent checkbox)
	{
		return Sanitize(std::move(checkbox));
	}

	bool IsValidUICallbackName(std::string_view name)
	{
		static constexpr std::string_view Keywords[] = { "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while" };
		if (name.empty() || name.size() > MaxUICallbackNameLength)
			return false;
		const auto isAlpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
		if (!isAlpha(name.front()))
			return false;
		for (char c : name)
		{
			if (!isAlpha(c) && !(c >= '0' && c <= '9'))
				return false;
		}
		return std::ranges::find(Keywords, name) == std::end(Keywords);
	}

	float ClampOrFallback(float value, float min, float max, float fallback)
	{
		return std::isfinite(value) ? std::clamp(value, min, max) : fallback;
	}

	float UIAlignFraction(UIHorizontalAlign align)
	{
		switch (align)
		{
			case UIHorizontalAlign::Left:
				return 0.0f;
			case UIHorizontalAlign::Center:
				return 0.5f;
			case UIHorizontalAlign::Right:
				return 1.0f;
		}
		return 0.0f; // out-of-range values (only C++ can write them)
	}

	float UIAlignFraction(UIVerticalAlign align)
	{
		switch (align)
		{
			case UIVerticalAlign::Top:
				return 0.0f;
			case UIVerticalAlign::Middle:
				return 0.5f;
			case UIVerticalAlign::Bottom:
				return 1.0f;
		}
		return 0.0f; // out-of-range values (only C++ can write them)
	}

}
