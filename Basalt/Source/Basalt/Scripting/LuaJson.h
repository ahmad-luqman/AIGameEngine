#pragma once

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

namespace Basalt {

	// Conversions between Lua values and JSON, used for component access from Lua and script properties.
	// JSON arrays become 1-based Lua sequences; Vec2/3/4 userdata become arrays; Quat becomes Euler
	// degrees [x, y, z]; Entity userdata becomes its numeric ID.
	sol::object JsonToLua(sol::state_view lua, const nlohmann::json& value);
	// Throws std::runtime_error for values that cannot be represented (functions, cycles, deep nesting).
	nlohmann::json LuaToJson(const sol::object& value);

}
