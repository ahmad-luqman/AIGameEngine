#include "Basalt/Scripting/ScriptUI.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scene/Scene.h"
#include "Basalt/Scripting/ScriptGlue.h"
#include "Basalt/UI/GameUI.h"
#include "Basalt/UI/UIInteraction.h"
#include "Basalt/UI/UILayout.h"
#include "Basalt/UI/UIWidgets.h"
#include "Basalt/UI/UIRenderer.h"

#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Basalt {

	namespace {

		// Raises the Lua errors of one UI.* call, naming the function. Values outside the ranges the UI
		// components accept are errors too, rather than silently clamped.
		class Checker
		{
		public:
			explicit Checker(const char* function)
				: m_Function(function)
			{
			}

			[[noreturn]] void Fail(const std::string& message) const { throw std::runtime_error(fmt::format("{}: {}", m_Function, message)); }

			float Range(const char* name, float value, float min, float max) const
			{
				// Written so NaN fails too.
				if (!(value >= min && value <= max))
					Fail(fmt::format("{} must be between {} and {} (got {})", name, min, max, value));
				return value;
			}

			template<glm::length_t L>
			glm::vec<L, float, glm::defaultp> Range(const char* name, const glm::vec<L, float, glm::defaultp>& value, float min, float max) const
			{
				constexpr const char* Components[] = { "x", "y", "z", "w" };
				for (glm::length_t i = 0; i < L; i++)
				{
					if (!(value[i] >= min && value[i] <= max))
						Fail(fmt::format("{}.{} must be between {} and {} (got {})", name, Components[i], min, max, value[i]));
				}
				return value;
			}

			// Display colors and opacity, 0..1.
			glm::vec4 Color(const char* name, const glm::vec4& value) const { return Range(name, value, 0.0f, 1.0f); }

			// A position on the canvas, or an offset from an anchor.
			glm::vec2 Position(float x, float y) const
			{
				constexpr float Limit = UIRectComponent::MaxMagnitude;
				return { Range("x", x, -Limit, Limit), Range("y", y, -Limit, Limit) };
			}

			// A negative size draws nothing (as for a health bar at negative health: SanitizeUIDrawCommand makes
			// it 0); NaN and sizes beyond the canvas limits are errors.
			glm::vec2 Size(float width, float height) const
			{
				constexpr float Limit = UIRectComponent::MaxMagnitude;
				return { Range("width", width, -Limit, Limit), Range("height", height, -Limit, Limit) };
			}

		private:
			const char* m_Function;
		};

		// Reads one UI.* options table strictly: a key that is not an option of the function, or a value of
		// the wrong type or out of range, is a Lua error naming the function and the key.
		class Options
		{
		public:
			Options(Checker checker, sol::table table, std::initializer_list<std::string_view> allowed)
				: Options(checker, std::move(table), std::span<const std::string_view>(allowed.begin(), allowed.size()))
			{
			}

			Options(Checker checker, sol::table table, std::span<const std::string_view> allowed)
				: m_Checker(checker)
				, m_Table(std::move(table))
			{
				for (const auto& [key, value] : m_Table)
				{
					if (key.get_type() != sol::type::string)
						m_Checker.Fail("option keys must be names");
					const std::string name = key.as<std::string>();
					if (std::ranges::find(allowed, std::string_view(name)) == allowed.end())
						m_Checker.Fail(fmt::format("unknown option '{}' (options: {})", name, fmt::join(allowed, ", ")));
				}
			}

			std::optional<float> Number(const char* key) const { return Typed<float>(key, sol::type::number, "a number"); }
			std::optional<bool> Bool(const char* key) const { return Typed<bool>(key, sol::type::boolean, "true or false"); }
			std::optional<std::string> String(const char* key) const { return Typed<std::string>(key, sol::type::string, "a string"); }

			std::optional<float> Number(const char* key, float min, float max) const
			{
				std::optional<float> value = Number(key);
				if (value)
					m_Checker.Range(key, *value, min, max);
				return value;
			}

			// Vec2/Vec4 userdata, each component within [min, max].
			template<typename T>
			std::optional<T> Vector(const char* key, const char* typeName, float min, float max) const
			{
				const sol::object value = m_Table[key];
				if (value == sol::lua_nil)
					return std::nullopt;
				if (!value.is<T>())
					m_Checker.Fail(fmt::format("option '{}' must be a {}", key, typeName));
				return m_Checker.Range(key, value.as<T>(), min, max);
			}

			// Display colors and opacity, 0..1.
			std::optional<glm::vec4> Color(const char* key) const { return Vector<glm::vec4>(key, "Vec4", 0.0f, 1.0f); }

			// An anchor preset name ("TopLeft", "Center", ...) or a Vec2 of canvas fractions.
			std::optional<glm::vec2> Anchor(const char* key) const
			{
				const sol::object value = m_Table[key];
				if (value == sol::lua_nil)
					return std::nullopt;
				if (value.get_type() == sol::type::string)
				{
					const std::string name = value.as<std::string>();
					if (std::optional<glm::vec2> anchor = FindUIAnchorPreset(name))
						return anchor;
					m_Checker.Fail(fmt::format("unknown {} '{}' (presets: {})", key, name, fmt::join(GetUIAnchorPresetNames(), ", ")));
				}
				if (!value.is<glm::vec2>())
					m_Checker.Fail(fmt::format("option '{}' must be a preset name or a Vec2", key));
				return m_Checker.Range(key, value.as<glm::vec2>(), 0.0f, 1.0f);
			}

		private:
			// Compares the Lua type itself, so "20" is not a number and 42 is not a string.
			template<typename T>
			std::optional<T> Typed(const char* key, sol::type type, const char* expected) const
			{
				const sol::object value = m_Table[key];
				if (value == sol::lua_nil)
					return std::nullopt;
				if (value.get_type() != type)
					m_Checker.Fail(fmt::format("option '{}' must be {}", key, expected));
				return value.as<T>();
			}

			Checker m_Checker;
			sol::table m_Table;
		};

		float ParseAlign(const std::string& name, const Checker& checker)
		{
			if (name == "Left")
				return 0.0f;
			if (name == "Center")
				return 0.5f;
			if (name == "Right")
				return 1.0f;
			checker.Fail(fmt::format("align must be 'Left', 'Center' or 'Right' (got '{}')", name));
		}

		// The optional last argument of UI.Rect / UI.Image: nil, a color (called `name`), or an options table.
		std::optional<sol::table> OptionsOrColor(const sol::object& argument, glm::vec4& color, const Checker& checker, const char* name)
		{
			if (argument == sol::lua_nil)
				return std::nullopt;
			if (argument.get_type() == sol::type::table)
				return argument.as<sol::table>();
			if (!argument.is<glm::vec4>())
				checker.Fail(fmt::format("the last argument must be a Vec4 {} or an options table", name));
			color = checker.Color(name, argument.as<glm::vec4>());
			return std::nullopt;
		}

		// Anchor and Pivot shared by every UI.* function: the pivot defaults to the anchor, so an element
		// anchored to a corner or edge grows away from it.
		void ReadPlacement(const Options& options, UIDrawCommand& command)
		{
			if (std::optional<glm::vec2> anchor = options.Anchor("Anchor"))
				command.Anchor = *anchor;
			command.Pivot = options.Vector<glm::vec2>("Pivot", "Vec2", 0.0f, 1.0f).value_or(command.Anchor);
			command.Rotation = glm::radians(options.Number("Rotation", -360000.0f, 360000.0f).value_or(0.0f));
			command.Scale = options.Vector<glm::vec2>("Scale", "Vec2", 0.001f, 1000.0f).value_or(command.Scale);
		}

		std::string TooManyCommandsMessage()
		{
			return fmt::format("more than {} UI commands this frame", GameUI::MaxCommands);
		}

		void Submit(UIDrawCommand command, const Checker& checker)
		{
			switch (GameUI::Submit(std::move(command)))
			{
				case GameUI::SubmitResult::Queued:
					return;
				case GameUI::SubmitResult::InvalidPlacement:
					// The checks above reject every such value.
					BS_CORE_ASSERT(false, "a UI.* function let a non-finite placement through");
					checker.Fail("positions, sizes, anchors and pivots must be finite numbers");
				case GameUI::SubmitResult::QueueFull:
					// Most likely a loop that never ends; dropping later (top-most) UI silently would hide it.
					checker.Fail(TooManyCommandsMessage());
			}
		}

		// Options every immediate-mode widget takes, besides its own.
		constexpr std::string_view WidgetOptions[] = { "Id", "Anchor", "Pivot", "Interactable", "Color", "TextColor", "Size", "Font" };

		std::vector<std::string_view> WidgetOptionNames(std::initializer_list<std::string_view> own)
		{
			std::vector<std::string_view> names(std::begin(WidgetOptions), std::end(WidgetOptions));
			names.insert(names.end(), own.begin(), own.end());
			return names;
		}

		// The play session's widget input, or nullptr when the scene does not play.
		UIInteraction* FindInteraction(ScriptContext& context)
		{
			return context.SceneContext ? context.SceneContext->GetUIInteraction() : nullptr;
		}

		// Immediate-mode widgets only exist while the scene plays.
		UIInteraction& RequireInteraction(ScriptContext& context, const Checker& checker)
		{
			UIInteraction* interaction = FindInteraction(context);
			if (!interaction)
				checker.Fail("widgets only work while the scene plays");
			return *interaction;
		}

		// One immediate-mode widget call: reads the shared arguments and options (and a slider's handle width), then
		// registers the widget for input and draws its parts and label. Use: construct, Register once, then Draw.
		class ImmediateWidget
		{
		public:
			// Draw commands one widget may submit: the most is a slider's track, fill and handle
			// (UIWidgets::AppendSlider) and its label. Change it with them.
			static constexpr size_t MaxCommands = 4;

			ImmediateWidget(const Checker& checker, UIWidgetKind kind, const std::string& label, float x, float y, float width, float height, const sol::object& optionsArgument, std::initializer_list<std::string_view> ownOptions)
				: m_Checker(checker)
				, m_Label(label)
			{
				m_Widget.Kind = kind;
				const glm::vec2 position = checker.Position(x, y);
				// Unlike UI.Rect, a widget needs an area to take input in.
				const glm::vec2 size = checker.Size(width, height);
				if (!(size.x > 0.0f && size.y > 0.0f))
					checker.Fail(fmt::format("width and height must be positive (got {} x {})", size.x, size.y));
				m_Widget.Size = size;
				m_FontSize = std::clamp(size.y * 0.6f, UITextComponent::MinFontSize, UITextComponent::DefaultFontSize);
				std::string id = label;
				glm::vec2 pivot = { 0.0f, 0.0f };
				if (optionsArgument.get_type() == sol::type::table)
				{
					const std::vector<std::string_view> names = WidgetOptionNames(ownOptions);
					m_Options.emplace(checker, optionsArgument.as<sol::table>(), names);
					const Options& options = *m_Options;
					if (std::optional<std::string> value = options.String("Id"))
					{
						if (value->empty())
							checker.Fail("option 'Id' must not be empty");
						id = std::move(*value);
					}
					UIDrawCommand placement;
					ReadPlacement(options, placement);
					m_Widget.Anchor = placement.Anchor;
					pivot = placement.Pivot;
					m_Widget.Interactable = options.Bool("Interactable").value_or(true);
					m_Color = options.Color("Color");
					m_TextColor = options.Color("TextColor").value_or(m_TextColor);
					m_FontSize = options.Number("Size", UITextComponent::MinFontSize, UITextComponent::MaxFontSize).value_or(m_FontSize);
					m_Font = options.String("Font").value_or("");
				}
				else if (optionsArgument != sol::lua_nil)
				{
					checker.Fail("the last argument must be an options table");
				}
				if (id.empty())
					checker.Fail("a widget needs a label or an 'Id' option to tell it apart from others");
				m_Widget.ID = std::move(id);
				m_Widget.Offset = position - pivot * size;
				if (kind == UIWidgetKind::Slider)
				{
					m_Widget.HandleWidth = OptionNumber("HandleWidth", 0.0f, UIRectComponent::MaxMagnitude, UISliderComponent{}.HandleWidth);
					m_Widget.Vertical = m_Options ? m_Options->Bool("Vertical").value_or(false) : false;
				}
			}

			glm::vec4 Color(const glm::vec4& fallback) const { return m_Color.value_or(fallback); }
			// A widget's own option, or `fallback` when it is not given.
			// Read before Register: a bad option is an error, and the widget must not be registered by then.
			glm::vec4 OptionColor(const char* key, const glm::vec4& fallback) const
			{
				BS_CORE_ASSERT(!m_Registered, "options are read before Register");
				return m_Options ? m_Options->Color(key).value_or(fallback) : fallback;
			}
			float OptionNumber(const char* key, float min, float max, float fallback) const
			{
				BS_CORE_ASSERT(!m_Registered, "options are read before Register");
				return m_Options ? m_Options->Number(key, min, max).value_or(fallback) : fallback;
			}
			float GetHandleWidth() const { return m_Widget.HandleWidth; }
			bool IsVertical() const { return m_Widget.Vertical; }
			UIBox Rect() const { return { m_Widget.Offset, m_Widget.Size }; }

			// Registers the widget for input and returns what the user did to it this frame (nothing when it is drawn
			// disabled). Fails before registering when its drawing would not fit in this frame's commands, so a
			// widget is never registered without being drawn.
			UIImmediateInput Register(ScriptContext& context)
			{
				BS_CORE_ASSERT(!m_Registered, "an immediate-mode widget registers once");
				m_Registered = true;
				m_Widget.Clip = GameUI::GetClip();
				UIInteraction& interaction = RequireInteraction(context, m_Checker);
				if (GameUI::GetCommandCount() + MaxCommands > GameUI::MaxCommands)
					m_Checker.Fail(TooManyCommandsMessage());
				UIImmediateInput input;
				switch (interaction.SubmitImmediate(m_Widget, input))
				{
					case UIInteraction::SubmitResult::Accepted:
						return input;
					case UIInteraction::SubmitResult::DuplicateID:
						m_Checker.Fail(fmt::format("another widget already uses the id '{}' this frame (give one of them an 'Id' option)", m_Widget.ID));
					case UIInteraction::SubmitResult::TooMany:
						m_Checker.Fail(fmt::format("more than {} widgets this frame", UIInteraction::MaxImmediateWidgets));
				}
				BS_CORE_ASSERT(false, "unknown SubmitResult");
				return input;
			}

			// Submits draw commands built on the widget's rect (UIWidgets::Append*), anchored like the widget, then
			// the label in `labelBox` at `labelAlign` (nothing for an empty label).
			void Draw(std::vector<UIDrawCommand> commands, const UIBox& labelBox, const glm::vec2& labelAlign) const
			{
				BS_CORE_ASSERT(m_Registered, "a widget registers before it draws");
				if (!m_Label.empty())
				{
					UIDrawCommand& label = commands.emplace_back();
					label.Kind = UIDrawKind::Text;
					label.Position = labelBox.Min;
					label.Size = labelBox.Size;
					label.Text = m_Label;
					label.Font = m_Font;
					label.FontSize = m_FontSize;
					label.Color = m_TextColor;
					label.Align = labelAlign;
				}
				BS_CORE_ASSERT(commands.size() <= MaxCommands, "Register reserved room for every command");
				for (UIDrawCommand& command : commands)
				{
					command.Anchor = m_Widget.Anchor;
					Submit(std::move(command), m_Checker);
				}
			}

		private:
			const Checker& m_Checker;
			std::string m_Label;
			UIImmediateWidget m_Widget;
			std::optional<Options> m_Options;
			std::optional<glm::vec4> m_Color;
			glm::vec4 m_TextColor = { 1.0f, 1.0f, 1.0f, 1.0f };
			float m_FontSize = UITextComponent::DefaultFontSize;
			std::string m_Font;
			bool m_Registered = false;
		};

		// The tints of immediate-mode widgets: the components' defaults.
		template<typename T>
		glm::vec4 DefaultTint(UIWidgetState state)
		{
			return UIWidgetTints::Of(T{}).For(state);
		}

	}

	void RegisterUIBindings(sol::state& lua, ScriptContext& context)
	{
		sol::table ui = lua.create_named_table("UI");

		ui["GetSize"] = []() { return GameUI::GetCanvasSize(); };
		ui["PushClip"] = [](float x, float y, float width, float height) {
			const Checker checker("UI.PushClip");
			const glm::vec2 size = checker.Size(width, height);
			if (size.x < 0 || size.y < 0 || !GameUI::PushClip({ checker.Position(x, y), size }))
				checker.Fail("invalid clip or clip stack exceeds 256 levels");
		};
		ui["PopClip"] = []() { if (!GameUI::PopClip()) Checker("UI.PopClip").Fail("clip stack is empty"); };
		ui["MeasureText"] = [](const std::string& text, const sol::object& sizeOrOptions) {
			const Checker checker("UI.MeasureText");
			float size = UITextComponent::DefaultFontSize, width = 0;
			std::string font;
			if (sizeOrOptions.get_type() == sol::type::table)
			{
				const Options options(checker, sizeOrOptions.as<sol::table>(), { "Size", "Font", "Width" });
				size = options.Number("Size", UITextComponent::MinFontSize, UITextComponent::MaxFontSize).value_or(size);
				width = options.Number("Width", 0.0f, UIRectComponent::MaxMagnitude).value_or(width);
				font = options.String("Font").value_or(font);
			}
			else if (sizeOrOptions.get_type() == sol::type::number)
				size = checker.Range("size", sizeOrOptions.as<float>(), UITextComponent::MinFontSize, UITextComponent::MaxFontSize);
			else if (sizeOrOptions != sol::lua_nil)
				checker.Fail("the second argument must be a size or options table");
			return UIRenderer::MeasureText(text, size, font, width);
		};

		// UI.Text(text, x, y [, size [, color [, align]]]) or UI.Text(text, x, y, options).
		ui["Text"] = [](const std::string& text, float x, float y, const sol::object& sizeOrOptions, sol::optional<glm::vec4> color, sol::optional<std::string> align) {
			const Checker checker("UI.Text");
			UIDrawCommand command;
			command.Kind = UIDrawKind::Text;
			command.Text = text;
			command.Position = checker.Position(x, y);
			if (color)
				command.Color = checker.Color("color", *color);
			std::optional<float> alignX;
			if (align)
				alignX = ParseAlign(*align, checker);
			constexpr float MinSize = UITextComponent::MinFontSize;
			constexpr float MaxSize = UITextComponent::MaxFontSize;
			if (sizeOrOptions.get_type() == sol::type::table)
			{
				if (color || align)
					checker.Fail("pass either an options table or size, color and align, not both");
				const Options options(checker, sizeOrOptions.as<sol::table>(), { "Size", "Color", "Align", "Anchor", "Pivot", "Font", "Shadow", "Width", "Rotation", "Scale" });
				command.FontSize = options.Number("Size", MinSize, MaxSize).value_or(command.FontSize);
				command.Color = options.Color("Color").value_or(command.Color);
				if (std::optional<std::string> name = options.String("Align"))
					alignX = ParseAlign(*name, checker);
				ReadPlacement(options, command);
				command.Font = options.String("Font").value_or(command.Font);
				command.Shadow = options.Bool("Shadow").value_or(command.Shadow);
				if (std::optional<float> width = options.Number("Width"))
				{
					if (!(*width > 0.0f && *width <= UIRectComponent::MaxMagnitude))
						checker.Fail(fmt::format("option 'Width' must be positive and at most {} (got {})", UIRectComponent::MaxMagnitude, *width));
					command.Size.x = *width;
					command.Wrap = true;
				}
			}
			else if (sizeOrOptions.get_type() == sol::type::number)
			{
				command.FontSize = checker.Range("size", sizeOrOptions.as<float>(), MinSize, MaxSize);
			}
			else if (sizeOrOptions != sol::lua_nil)
			{
				checker.Fail("the fourth argument must be a size or an options table");
			}
			// Text is placed by its pivot; Align overrides the pivot's x.
			if (alignX)
				command.Pivot.x = *alignX;
			command.Align = command.Pivot;
			Submit(std::move(command), checker);
		};

		// UI.Rect(x, y, width, height [, color | options]).
		ui["Rect"] = [](float x, float y, float width, float height, const sol::object& colorOrOptions) {
			const Checker checker("UI.Rect");
			UIDrawCommand command;
			command.Kind = UIDrawKind::Rect;
			command.Position = checker.Position(x, y);
			command.Size = checker.Size(width, height);
			command.Color = glm::vec4(0.0f, 0.0f, 0.0f, 0.5f);
			if (std::optional<sol::table> table = OptionsOrColor(colorOrOptions, command.Color, checker, "color"))
			{
				const Options options(checker, *table, { "Color", "Anchor", "Pivot", "Rotation", "Scale" });
				command.Color = options.Color("Color").value_or(command.Color);
				ReadPlacement(options, command);
			}
			Submit(std::move(command), checker);
		};

		// UI.Image(texture, x, y, width, height [, tint | options]).
		ui["Image"] = [](const std::string& texture, float x, float y, float width, float height, const sol::object& tintOrOptions) {
			const Checker checker("UI.Image");
			if (texture.empty())
				checker.Fail("the texture path is empty (use UI.Rect for a plain rectangle)");
			UIDrawCommand command;
			command.Kind = UIDrawKind::Image;
			command.Texture = texture;
			command.Position = checker.Position(x, y);
			command.Size = checker.Size(width, height);
			if (std::optional<sol::table> table = OptionsOrColor(tintOrOptions, command.Color, checker, "tint"))
			{
				const Options options(checker, *table, { "Tint", "Anchor", "Pivot", "Slice", "SliceScale", "UVMin", "UVMax", "SpriteColumns", "SpriteRows", "SpriteFrame", "Rotation", "Scale" });
				command.Color = options.Color("Tint").value_or(command.Color);
				ReadPlacement(options, command);
				command.Slice = options.Vector<glm::vec4>("Slice", "Vec4 (left, top, right, bottom texels)", 0.0f, UIImageComponent::MaxSlice).value_or(command.Slice);
				command.SliceScale = options.Number("SliceScale", UIImageComponent::MinSliceScale, UIImageComponent::MaxSliceScale).value_or(command.SliceScale);
				command.UVMin = options.Vector<glm::vec2>("UVMin", "Vec2", 0, 1).value_or(command.UVMin);
				command.UVMax = options.Vector<glm::vec2>("UVMax", "Vec2", 0, 1).value_or(command.UVMax);
				const auto integer = [&](const char* name, uint32_t fallback, uint32_t max) {
					const float value = options.Number(name, name == std::string_view("SpriteFrame") ? 0.0f : 1.0f, static_cast<float>(max)).value_or(static_cast<float>(fallback));
					if (std::floor(value) != value)
						checker.Fail(fmt::format("{} must be an integer", name));
					return static_cast<uint32_t>(value);
				};
				command.SpriteColumns = integer("SpriteColumns", 1, 4096);
				command.SpriteRows = integer("SpriteRows", 1, 4096);
				command.SpriteFrame = integer("SpriteFrame", 0, command.SpriteColumns * command.SpriteRows - 1);
			}
			Submit(std::move(command), checker);
		};

		// UI.Button(label, x, y, width, height [, options]): true on the frame the button is clicked.
		ui["Button"] = [&context](const std::string& label, float x, float y, float width, float height, const sol::object& options) {
			const Checker checker("UI.Button");
			ImmediateWidget widget(checker, UIWidgetKind::Button, label, x, y, width, height, options, {});
			const UIImmediateInput input = widget.Register(context);
			std::vector<UIDrawCommand> commands;
			UIWidgets::AppendRect(widget.Rect(), widget.Color({ 0.18f, 0.2f, 0.26f, 0.92f }) * DefaultTint<UIButtonComponent>(input.State), commands);
			widget.Draw(std::move(commands), widget.Rect(), { 0.5f, 0.5f });
			return input.Activated;
		};

		// UI.Checkbox(label, checked, x, y, width, height [, options]): the new checked state.
		ui["Checkbox"] = [&context](const std::string& label, bool checked, float x, float y, float width, float height, const sol::object& options) {
			const Checker checker("UI.Checkbox");
			ImmediateWidget widget(checker, UIWidgetKind::Checkbox, label, x, y, width, height, options, { "CheckColor" });
			const UICheckboxComponent defaults;
			const glm::vec4 checkColor = widget.OptionColor("CheckColor", defaults.CheckColor);
			const UIImmediateInput input = widget.Register(context);
			if (input.Activated)
				checked = !checked;
			std::vector<UIDrawCommand> commands;
			UIWidgets::AppendCheckbox(widget.Rect(), checked, widget.Color(defaults.BoxColor) * DefaultTint<UICheckboxComponent>(input.State), checkColor, commands);
			widget.Draw(std::move(commands), UIWidgets::CheckboxLabelBox(widget.Rect()), { 0.0f, 0.5f });
			return checked;
		};

		// UI.Slider(label, value, min, max, x, y, width, height [, options]): the new value.
		ui["Slider"] = [&context](const std::string& label, float value, float min, float max, float x, float y, float width, float height, const sol::object& options) {
			const Checker checker("UI.Slider");
			constexpr float Limit = UIRectComponent::MaxMagnitude;
			checker.Range("min", min, -Limit, Limit);
			checker.Range("max", max, -Limit, Limit);
			if (!(max > min))
				checker.Fail(fmt::format("max must be greater than min (got min {} and max {})", min, max));
			checker.Range("value", value, min, max);
			ImmediateWidget widget(checker, UIWidgetKind::Slider, label, x, y, width, height, options, { "Step", "FillColor", "HandleColor", "HandleWidth", "Vertical" });
			const UISliderComponent defaults;
			const float step = widget.OptionNumber("Step", 0.0f, Limit, defaults.Step);
			const glm::vec4 fillColor = widget.OptionColor("FillColor", defaults.FillColor);
			const glm::vec4 handleColor = widget.OptionColor("HandleColor", defaults.HandleColor);
			const UIImmediateInput input = widget.Register(context);
			if (input.SliderFraction)
				value = UIWidgets::SliderValueAtFraction(*input.SliderFraction, min, max, step);
			if (input.SliderSteps != 0)
				value = UIWidgets::StepSliderValue(value, input.SliderSteps, min, max, step);
			std::vector<UIDrawCommand> commands;
			UIWidgets::AppendSlider(widget.Rect(), UIWidgets::SliderFraction(value, min, max), widget.GetHandleWidth(), widget.Color(defaults.TrackColor), fillColor, handleColor * DefaultTint<UISliderComponent>(input.State), commands, widget.IsVertical());
			widget.Draw(std::move(commands), widget.Rect(), { 0.5f, 0.5f });
			return value;
		};

		// UI.GetFocus(): the focused widget: an entity (widget component), an id string (immediate-mode widget) or
		// nil.
		ui["GetFocus"] = [&context](sol::this_state state) -> sol::object {
			const UIInteraction* interaction = FindInteraction(context);
			if (!interaction)
				return sol::lua_nil;
			const UIWidgetID& focus = interaction->GetFocus();
			if (focus.IsEntity())
				return sol::make_object(state, ScriptEntity{ focus.GetEntity(), context.SceneContext });
			if (focus.IsName())
				return sol::make_object(state, focus.GetName());
			return sol::lua_nil;
		};

		// UI.SetFocus(widget): focuses a widget entity or an immediate-mode widget id; nil clears the focus.
		ui["SetFocus"] = [&context](const sol::object& target) {
			const Checker checker("UI.SetFocus");
			UIInteraction& interaction = RequireInteraction(context, checker);
			if (target == sol::lua_nil)
			{
				interaction.SetFocus({});
				return;
			}
			if (target.get_type() == sol::type::string)
			{
				std::string id = target.as<std::string>();
				if (id.empty())
					checker.Fail("the widget id must not be empty");
				interaction.SetFocus(UIWidgetID::ForName(std::move(id)));
				return;
			}
			if (!target.is<ScriptEntity>())
				checker.Fail("the argument must be a widget entity, a widget id or nil");
			Entity entity = target.as<ScriptEntity>().ResolveOrThrow();
			const auto kind = UILayout::GetWidgetKind(entity);
			if (!kind || *kind == UIWidgetKind::Panel || *kind == UIWidgetKind::ScrollView)
				checker.Fail(fmt::format("'{}' has no UIButton, UISlider, UICheckbox, UITextInput or UIDropdown", entity.GetName()));
			interaction.SetFocus(UIWidgetID::ForEntity(entity.GetUUID()));
		};

		// UI.IsPointerOverWidget(): whether the pointer is over an interactable widget (a click there is the UI's).
		ui["IsPointerOverWidget"] = [&context]() {
			const UIInteraction* interaction = FindInteraction(context);
			return interaction && interaction->IsPointerOverWidget();
		};

		// UI.SetNavigationEnabled(enabled): whether the keyboard and gamepads move the focus and activate widgets (a
		// game that reads them itself turns it off while widgets are visible).
		ui["SetNavigationEnabled"] = [&context](bool enabled) { RequireInteraction(context, Checker("UI.SetNavigationEnabled")).SetNavigationEnabled(enabled); };
		ui["IsNavigationEnabled"] = [&context]() {
			const UIInteraction* interaction = FindInteraction(context);
			return interaction && interaction->IsNavigationEnabled();
		};

		// UI.GetRect(entity): the laid-out rect of an entity with UI components as Vec4(x, y, width, height)
		// in canvas units, or nil when it has none or it or an ancestor is hidden. Laid out for the last
		// rendered viewport (1920x1080 headless).
		ui["GetRect"] = [](const ScriptEntity& self) -> std::optional<glm::vec4> {
			Entity entity = self.ResolveOrThrow();
			const glm::vec2 canvas = GameUI::GetCanvasSize();
			const std::optional<UIBox> rect = UILayout::GetRect(*entity.GetScene(), entity, canvas);
			if (!rect)
				return std::nullopt;
			return glm::vec4(rect->Min, rect->Size);
		};
	}

}
