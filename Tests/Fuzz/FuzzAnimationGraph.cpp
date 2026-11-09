// Animation graph assets are editable JSON and must reject invalid references and types without throwing.
#include "FuzzCommon.h"

#include <Breccia/Animation/AnimationGraph.h>
#include <Breccia/Core/JsonUtils.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BrecciaFuzz::Init();
	std::string error;
	const auto parsed = Breccia::ParseJson(BrecciaFuzz::AsText(data, size), error);
	if (!parsed.is_discarded())
		Breccia::AnimationGraph::FromJson(parsed, error);
	return 0;
}
