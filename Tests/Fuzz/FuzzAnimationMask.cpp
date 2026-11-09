// Animation mask assets are editable JSON and must reject invalid joints and weights without throwing.
#include "FuzzCommon.h"

#include <Breccia/Animation/AnimationMask.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BrecciaFuzz::Init();
	std::string error;
	Breccia::AnimationMask::FromText(BrecciaFuzz::AsText(data, size), error);
	return 0;
}
