// Input action files (Input.binput): players and modders can edit them, so they are untrusted input.
#include "FuzzCommon.h"

#include <Basalt/Core/InputActions.h>
#include <Basalt/Core/JsonUtils.h>

#include <nlohmann/json.hpp>

#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	const nlohmann::json parsed = Basalt::ParseJson(BasaltFuzz::AsText(data, size));
	if (parsed.is_discarded())
		return 0;
	std::string error;
	if (const auto map = Basalt::InputActionMap::FromJson(parsed, error))
	{
		// Whatever loads must save and load again unchanged, and evaluate without input.
		const nlohmann::json saved = map->ToJson();
		const auto reloaded = Basalt::InputActionMap::FromJson(saved, error);
		if (!reloaded || *reloaded != *map || reloaded->ToJson() != saved)
			__builtin_trap();
		for (const Basalt::InputAction& action : map->GetActions())
		{
			if (Basalt::InputActionMap::GetValue(action) != 0.0f || Basalt::InputActionMap::IsPressed(action))
				__builtin_trap();
		}
	}
	return 0;
}
