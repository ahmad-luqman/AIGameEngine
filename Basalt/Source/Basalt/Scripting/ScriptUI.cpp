#include "Basalt/Scripting/ScriptUI.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Scene/Entity.h"
#include "Basalt/Scripting/ScriptGlue.h"
#include "Basalt/UI/GameUI.h"
#include "Basalt/UI/UILayout.h"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Basalt {

	namespace {

		std::string JoinNames(std::span<const std::string_view> names)
		{
			std::string joined;
			for (std::string_view name : names)
			{
				if (!joined.empty())
					joined += ", ";
				joined += name;
			}
			return joined;
		}

		// Reads one UI.* options table strictly: a key that is not an option of the function, or a value of
		// the wrong type, is a Lua error naming the function and the key.
		class Options
		{
		public:
			Options(const char* function, sol::table table, std::initializer_list<std::string_view> allowed)
				: m_Function(function)
				, m_Table(std::move(table))
			{
				for (const auto& [key, value] : m_Table)
				{
					if (key.get_type() != sol::type::string)
						Fail("option keys must be names");
					const std::string name = key.as<std::string>();
					if (std::ranges::find(allowed, std::string_view(name)) == allowed.end())
					{
						const std::vector<std::string_view> names(allowed);
						Fail(fmt::format("unknown option '{}' (options: {})", name, JoinNames(names)));
					}
				}
			}

			std::optional<float> Number(const char* key) const
			{
				const sol::object value = m_Table[key];
				if (value == sol::lua_nil)
					return std::nullopt;
				if (value.get_type() != sol::type::number)
					Fail(fmt::format("option '{}' must be a number", key));
				return value.as<float>();
			}

			std::optional<bool> Bool(const char* key) const
			{
				const sol::object value = m_Table[key];
				if (value == sol::lua_nil)
					return std::nullopt;
				if (value.get_type() != sol::type::boolean)
					Fail(fmt::format("option '{}' must be true or false", key));
				return value.as<bool>();
			}

			std::optional<std::string> String(const char* key) const
			{
				const sol::object value = m_Table[key];
				if (value == sol::lua_nil)
					return std::nullopt;
				if (value.get_type() != sol::type::string)
					Fail(fmt::format("option '{}' must be a string", key));
				return value.as<std::string>();
			}

			template<typename T>
			std::optional<T> Value(const char* key, const char* typeName) const
			{
				const sol::object value = m_Table[key];
				if (value == sol::lua_nil)
					return std::nullopt;
				if (!value.is<T>())
					Fail(fmt::format("option '{}' must be a {}", key, typeName));
				return value.as<T>();
			}

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
					Fail(fmt::format("unknown {} '{}' (presets: {})", key, name, JoinNames(GetUIAnchorPresetNames())));
				}
				if (!value.is<glm::vec2>())
					Fail(fmt::format("option '{}' must be a preset name or a Vec2", key));
				return value.as<glm::vec2>();
			}

			[[noreturn]] void Fail(const std::string& message) const { throw std::runtime_error(fmt::format("{}: {}", m_Function, message)); }

		private:
			const char* m_Function;
			sol::table m_Table;
		};

		float ParseAlign(const std::string& name, const char* function)
		{
			if (name == "Left")
				return 0.0f;
			if (name == "Center")
				return 0.5f;
			if (name == "Right")
				return 1.0f;
			throw std::runtime_error(fmt::format("{} align must be 'Left', 'Center' or 'Right' (got '{}')", function, name));
		}

		// The optional last argument of UI.Rect / UI.Image: nil, a color, or an options table.
		std::optional<sol::table> OptionsOrColor(const sol::object& argument, glm::vec4& color, const char* function)
		{
			if (argument == sol::lua_nil)
				return std::nullopt;
			if (argument.get_type() == sol::type::table)
				return argument.as<sol::table>();
			if (!argument.is<glm::vec4>())
				throw std::runtime_error(fmt::format("{}: the last argument must be a Vec4 color or an options table", function));
			color = argument.as<glm::vec4>();
			return std::nullopt;
		}

		// Anchor and Pivot shared by every UI.* function: the pivot defaults to the anchor, so an element
		// anchored to a corner or edge grows away from it.
		void ReadPlacement(const Options& options, UIDrawCommand& command)
		{
			if (std::optional<glm::vec2> anchor = options.Anchor("Anchor"))
				command.Anchor = *anchor;
			command.Pivot = options.Value<glm::vec2>("Pivot", "Vec2").value_or(command.Anchor);
		}

		void Submit(UIDrawCommand command, const char* function)
		{
			if (!SanitizeUIDrawCommand(command))
				throw std::runtime_error(fmt::format("{}: positions, sizes, anchors and pivots must be finite numbers", function));
			GameUI::Submit(std::move(command));
		}

	}

	void RegisterUIBindings(sol::state& lua)
	{
		sol::table ui = lua.create_named_table("UI");

		ui["GetSize"] = []() { return GameUI::GetCanvasSize(GameUI::GetViewportSize()); };

		// UI.Text(text, x, y [, size [, color [, align]]]) or UI.Text(text, x, y, options).
		ui["Text"] = [](const std::string& text, float x, float y, const sol::object& sizeOrOptions, sol::optional<glm::vec4> color, sol::optional<std::string> align) {
			constexpr const char* function = "UI.Text";
			UIDrawCommand command;
			command.Kind = UIDrawKind::Text;
			command.Text = text;
			command.Position = { x, y };
			command.FontSize = 32.0f;
			command.Color = color.value_or(glm::vec4(1.0f));
			std::optional<float> alignX;
			if (align)
				alignX = ParseAlign(*align, function);
			if (sizeOrOptions.get_type() == sol::type::table)
			{
				if (color || align)
					throw std::runtime_error("UI.Text: pass either an options table or size, color and align, not both");
				const Options options(function, sizeOrOptions.as<sol::table>(), { "Size", "Color", "Align", "Anchor", "Pivot", "Font", "Shadow", "Width" });
				command.FontSize = options.Number("Size").value_or(command.FontSize);
				command.Color = options.Value<glm::vec4>("Color", "Vec4").value_or(command.Color);
				if (std::optional<std::string> name = options.String("Align"))
					alignX = ParseAlign(*name, function);
				ReadPlacement(options, command);
				command.Font = options.String("Font").value_or("");
				command.Shadow = options.Bool("Shadow").value_or(true);
				if (std::optional<float> width = options.Number("Width"))
				{
					if (!(*width > 0.0f))
						options.Fail("option 'Width' must be positive");
					command.Size.x = *width;
					command.Wrap = true;
				}
			}
			else if (sizeOrOptions.get_type() == sol::type::number)
			{
				command.FontSize = sizeOrOptions.as<float>();
			}
			else if (sizeOrOptions != sol::lua_nil)
			{
				throw std::runtime_error("UI.Text: the fourth argument must be a size or an options table");
			}
			// Text is placed by its pivot; Align overrides the pivot's x.
			if (alignX)
				command.Pivot.x = *alignX;
			command.Align = command.Pivot;
			Submit(std::move(command), function);
		};

		// UI.Rect(x, y, width, height [, color | options]).
		ui["Rect"] = [](float x, float y, float width, float height, const sol::object& colorOrOptions) {
			constexpr const char* function = "UI.Rect";
			UIDrawCommand command;
			command.Kind = UIDrawKind::Rect;
			command.Position = { x, y };
			command.Size = { width, height };
			command.Color = glm::vec4(0.0f, 0.0f, 0.0f, 0.5f);
			if (std::optional<sol::table> table = OptionsOrColor(colorOrOptions, command.Color, function))
			{
				const Options options(function, *table, { "Color", "Anchor", "Pivot" });
				command.Color = options.Value<glm::vec4>("Color", "Vec4").value_or(command.Color);
				ReadPlacement(options, command);
			}
			Submit(std::move(command), function);
		};

		// UI.Image(texture, x, y, width, height [, tint | options]).
		ui["Image"] = [](const std::string& texture, float x, float y, float width, float height, const sol::object& tintOrOptions) {
			constexpr const char* function = "UI.Image";
			if (texture.empty())
				throw std::runtime_error("UI.Image: the texture path is empty (use UI.Rect for a plain rectangle)");
			UIDrawCommand command;
			command.Kind = UIDrawKind::Image;
			command.Texture = texture;
			command.Position = { x, y };
			command.Size = { width, height };
			if (std::optional<sol::table> table = OptionsOrColor(tintOrOptions, command.Color, function))
			{
				const Options options(function, *table, { "Tint", "Anchor", "Pivot", "Slice", "SliceScale" });
				command.Color = options.Value<glm::vec4>("Tint", "Vec4").value_or(command.Color);
				ReadPlacement(options, command);
				command.Slice = options.Value<glm::vec4>("Slice", "Vec4 (left, top, right, bottom texels)").value_or(command.Slice);
				command.SliceScale = options.Number("SliceScale").value_or(command.SliceScale);
			}
			Submit(std::move(command), function);
		};

		// UI.GetRect(entity): the laid-out rect of an entity with UI components as Vec4(x, y, width, height)
		// in canvas units, or nil when it has none or is hidden.
		ui["GetRect"] = [](const ScriptEntity& self) -> std::optional<glm::vec4> {
			Entity entity = self.ResolveOrThrow();
			const glm::vec2 canvas = GameUI::GetCanvasSize(GameUI::GetViewportSize());
			const std::optional<UIBox> rect = UILayout::GetRect(*entity.GetScene(), entity, canvas);
			if (!rect)
				return std::nullopt;
			return glm::vec4(rect->Min, rect->Size);
		};
	}

}
