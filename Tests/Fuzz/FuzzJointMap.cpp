// Clip library joint map assets are editable JSON and must reject invalid or repeated joints without throwing.
#include "FuzzCommon.h"

#include <Breccia/Animation/AnimationJointMap.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BrecciaFuzz::Init();
	std::string error;
	Breccia::AnimationJointMap::FromText(BrecciaFuzz::AsText(data, size), error);
	return 0;
}
