// Animation mask assets are editable JSON and must reject invalid joints and weights without throwing.
#include "FuzzCommon.h"

#include <Basalt/Animation/AnimationMask.h>
#include <Basalt/Core/JsonUtils.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	std::string error;
	const auto parsed = Basalt::ParseJson(BasaltFuzz::AsText(data, size), error);
	if (!parsed.is_discarded())
		Basalt::AnimationMask::FromJson(parsed, error);
	return 0;
}
