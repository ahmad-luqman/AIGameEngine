// JSON <-> Lua conversion used for script properties and Lua GetComponent/SetComponent.
#include "FuzzCommon.h"

#include <Basalt/Core/JsonUtils.h>
#include <Basalt/Scripting/LuaJson.h>

#include <sol/sol.hpp>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	static sol::state lua;
	const nlohmann::json json = Basalt::ParseJson(BasaltFuzz::AsText(data, size));
	if (json.is_discarded())
		return 0;
	try
	{
		const sol::object object = Basalt::JsonToLua(lua, json);
		Basalt::LuaToJson(object);
	}
	// LuaToJson reports unsupported values (functions, too-deep tables) by throwing; callers catch it, and here an
	// expected rejection is not a finding.
	catch (const std::exception&) // NOLINT(bugprone-empty-catch)
	{
	}
	lua.collect_garbage();
	return 0;
}
