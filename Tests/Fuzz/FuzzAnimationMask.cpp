// Animation mask assets are editable JSON and must reject invalid joints and weights without throwing.
#include "FuzzCommon.h"

#include <Basalt/Animation/AnimationMask.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	std::string error;
	Basalt::AnimationMask::FromText(BasaltFuzz::AsText(data, size), error);
	return 0;
}
