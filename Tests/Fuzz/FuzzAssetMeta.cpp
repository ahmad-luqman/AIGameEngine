#include "FuzzCommon.h"

#include <Breccia/Asset/AssetRegistry.h>
#include <Breccia/Core/JsonUtils.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BrecciaFuzz::Init();
	std::string error;
	const auto parsed = Breccia::ParseJson(BrecciaFuzz::AsText(data, size), error);
	Breccia::AssetRegistry::ParseMeta(parsed, error);
	Breccia::AssetRegistry registry;
	std::string path;
	registry.Decode(parsed, path, error, true);
	nlohmann::json encoded;
	if (registry.Encode(path, encoded, error))
		(void)encoded.dump();
	return 0;
}
