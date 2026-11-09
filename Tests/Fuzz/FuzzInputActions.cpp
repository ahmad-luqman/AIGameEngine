// Input action files (Input.binput): players and modders can edit them, so they are untrusted input.
#include "FuzzCommon.h"

#include <Breccia/Core/InputActions.h>
#include <Breccia/Core/JsonUtils.h>

#include <nlohmann/json.hpp>

#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BrecciaFuzz::Init();
	// The reporting overload, as Project::ReadInputActions reads the file: it must never throw.
	std::string error;
	const nlohmann::json parsed = Breccia::ParseJson(BrecciaFuzz::AsText(data, size), error);
	if (parsed.is_discarded())
	{
		if (error.empty())
			__builtin_trap();
		return 0;
	}
	if (const auto map = Breccia::InputActionMap::FromJson(parsed, error))
	{
		// Whatever loads must save and load again unchanged, and evaluate without input.
		const nlohmann::json saved = map->ToJson();
		const auto reloaded = Breccia::InputActionMap::FromJson(saved, error);
		if (!reloaded || *reloaded != *map || reloaded->ToJson() != saved)
			__builtin_trap();
		for (const Breccia::InputAction& action : map->GetActions())
		{
			if (Breccia::InputActionMap::GetValue(action) != 0.0f || Breccia::InputActionMap::IsPressed(action))
				__builtin_trap();
		}
	}
	return 0;
}
