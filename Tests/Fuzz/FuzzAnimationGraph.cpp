// Animation graph assets are editable JSON and must reject invalid references and types without throwing.
#include "FuzzCommon.h"

#include <Basalt/Animation/AnimationGraph.h>
#include <Basalt/Core/JsonUtils.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	std::string error;
	const auto parsed = Basalt::ParseJson(BasaltFuzz::AsText(data, size), error);
	if (!parsed.is_discarded())
		Basalt::AnimationGraph::FromJson(parsed, error);
	return 0;
}
