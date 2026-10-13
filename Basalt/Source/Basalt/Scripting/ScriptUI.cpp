#include "Basalt/Scripting/ScriptUI.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scripting/ScriptGlue.h"
#include "Basalt/UI/GameUI.h"
#include "Basalt/UI/UILayout.h"

#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

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
					checker.Fail(fmt::format("more than {} UI commands this frame", GameUI::MaxCommands));
			}
		}

	}

	void RegisterUIBindings(sol::state& lua)
	{
		sol::table ui = lua.create_named_table("UI");

		ui["GetSize"] = []() { return GameUI::GetCanvasSize(); };

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
				const Options options(checker, sizeOrOptions.as<sol::table>(), { "Size", "Color", "Align", "Anchor", "Pivot", "Font", "Shadow", "Width" });
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
				const Options options(checker, *table, { "Color", "Anchor", "Pivot" });
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
				const Options options(checker, *table, { "Tint", "Anchor", "Pivot", "Slice", "SliceScale" });
				command.Color = options.Color("Tint").value_or(command.Color);
				ReadPlacement(options, command);
				command.Slice = options.Vector<glm::vec4>("Slice", "Vec4 (left, top, right, bottom texels)", 0.0f, UIImageComponent::MaxSlice).value_or(command.Slice);
				command.SliceScale = options.Number("SliceScale", UIImageComponent::MinSliceScale, UIImageComponent::MaxSliceScale).value_or(command.SliceScale);
			}
			Submit(std::move(command), checker);
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
