// Clip library joint map assets are editable JSON and must reject invalid or repeated joints without throwing.
#include "FuzzCommon.h"

#include <Basalt/Animation/AnimationJointMap.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	std::string error;
	Basalt::AnimationJointMap::FromText(BasaltFuzz::AsText(data, size), error);
	return 0;
}
