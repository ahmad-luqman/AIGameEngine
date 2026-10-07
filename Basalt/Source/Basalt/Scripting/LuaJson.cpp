#include "Basalt/Scripting/LuaJson.h"

#include "Basalt/Math/Math.h"
#include "Basalt/Scripting/ScriptGlue.h"

#include <glm/glm.hpp>

#include <stdexcept>

namespace Basalt {

	namespace {

		constexpr int MaxDepth = 32;

		nlohmann::json LuaToJsonImpl(const sol::object& value, int depth)
		{
			if (depth > MaxDepth)
				throw std::runtime_error("value is nested too deeply (or contains a cycle)");

			switch (value.get_type())
			{
				case sol::type::lua_nil:
				case sol::type::none:
					return nullptr;
				case sol::type::boolean:
					return value.as<bool>();
				case sol::type::number:
				{
					lua_State* state = value.lua_state();
					value.push(state);
					const bool isInteger = lua_isinteger(state, -1) != 0;
					lua_pop(state, 1);
					if (isInteger)
						return value.as<int64_t>();
					return value.as<double>();
				}
				case sol::type::string:
					return value.as<std::string>();
				case sol::type::userdata:
				{
					if (value.is<glm::vec2>())
					{
						const glm::vec2 v = value.as<glm::vec2>();
						return nlohmann::json::array({ v.x, v.y });
					}
					if (value.is<glm::vec3>())
					{
						const glm::vec3 v = value.as<glm::vec3>();
						return nlohmann::json::array({ v.x, v.y, v.z });
					}
					if (value.is<glm::vec4>())
					{
						const glm::vec4 v = value.as<glm::vec4>();
						return nlohmann::json::array({ v.x, v.y, v.z, v.w });
					}
					if (value.is<glm::quat>())
					{
						const glm::vec3 degrees = glm::degrees(Math::EulerFromQuat(value.as<glm::quat>()));
						return nlohmann::json::array({ degrees.x, degrees.y, degrees.z });
					}
					if (value.is<ScriptEntity>())
						return static_cast<uint64_t>(value.as<ScriptEntity>().ID);
					throw std::runtime_error("unsupported userdata value");
				}
				case sol::type::table:
				{
					const sol::table table = value.as<sol::table>();
					// Sequence (1..n with no holes) -> array, otherwise an object with string keys.
					size_t count = 0;
					bool sequence = true;
					table.for_each([&](const sol::object& key, const sol::object&) {
						count++;
						if (!key.is<int64_t>())
							sequence = false; });
					if (count > 0 && sequence)
					{
						for (size_t i = 1; i <= count; i++)
						{
							if (table[i].get_type() == sol::type::lua_nil)
								sequence = false;
						}
					}

					if (count > 0 && sequence)
					{
						nlohmann::json array = nlohmann::json::array();
						for (size_t i = 1; i <= count; i++)
							array.push_back(LuaToJsonImpl(table[i], depth + 1));
						return array;
					}

					nlohmann::json object = nlohmann::json::object();
					std::string error;
					table.for_each([&](const sol::object& key, const sol::object& element) {
						if (!error.empty())
							return;
						if (!key.is<std::string>())
						{
							error = "tables must use either sequence or string keys";
							return;
						}
						object[key.as<std::string>()] = LuaToJsonImpl(element, depth + 1); });
					if (!error.empty())
						throw std::runtime_error(error);
					return object;
				}
				default:
					throw std::runtime_error(std::string("cannot convert a Lua ") + sol::type_name(value.lua_state(), value.get_type()) + " to data");
			}
		}

	}

	sol::object JsonToLua(sol::state_view lua, const nlohmann::json& value)
	{
		switch (value.type())
		{
			case nlohmann::json::value_t::null:
			case nlohmann::json::value_t::discarded:
				return sol::lua_nil;
			case nlohmann::json::value_t::boolean:
				return sol::make_object(lua, value.get<bool>());
			case nlohmann::json::value_t::number_integer:
				return sol::make_object(lua, value.get<int64_t>());
			case nlohmann::json::value_t::number_unsigned:
				return sol::make_object(lua, static_cast<int64_t>(value.get<uint64_t>()));
			case nlohmann::json::value_t::number_float:
				return sol::make_object(lua, value.get<double>());
			case nlohmann::json::value_t::string:
				return sol::make_object(lua, value.get<std::string>());
			case nlohmann::json::value_t::array:
			{
				sol::table table = lua.create_table(static_cast<int>(value.size()), 0);
				for (size_t i = 0; i < value.size(); i++)
					table[i + 1] = JsonToLua(lua, value[i]);
				return table;
			}
			case nlohmann::json::value_t::object:
			{
				sol::table table = lua.create_table(0, static_cast<int>(value.size()));
				for (auto it = value.begin(); it != value.end(); ++it)
					table[it.key()] = JsonToLua(lua, it.value());
				return table;
			}
			case nlohmann::json::value_t::binary:
				break;
		}
		return sol::lua_nil;
	}

	nlohmann::json LuaToJson(const sol::object& value)
	{
		return LuaToJsonImpl(value, 0);
	}

}
