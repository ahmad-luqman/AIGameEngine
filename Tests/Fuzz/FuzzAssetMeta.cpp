#include "FuzzCommon.h"

#include <Basalt/Asset/AssetRegistry.h>
#include <Basalt/Core/JsonUtils.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	std::string error;
	const auto parsed = Basalt::ParseJson(BasaltFuzz::AsText(data, size), error);
	Basalt::AssetRegistry::ParseMeta(parsed, error);
	Basalt::AssetRegistry registry;
	std::string path;
	registry.Decode(parsed, path, error, true);
	nlohmann::json encoded;
	if (registry.Encode(path, encoded, error))
		(void)encoded.dump();
	return 0;
}
