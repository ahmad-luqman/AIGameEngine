// Save data files (Lua Storage): players can edit them, so they are untrusted input.
#include "FuzzCommon.h"

#include <Breccia/Project/SaveData.h>

#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BrecciaFuzz::Init();
	Breccia::SaveData saveData("FuzzSaveData.json");
	std::string error;
	if (saveData.LoadFromString(BrecciaFuzz::AsText(data, size), error))
	{
		// Whatever loads must save and load again unchanged.
		const std::string text = saveData.SaveToString();
		Breccia::SaveData reloaded("FuzzSaveData.json");
		if (!reloaded.LoadFromString(text, error) || reloaded.SaveToString() != text)
			__builtin_trap();
	}
	return 0;
}
