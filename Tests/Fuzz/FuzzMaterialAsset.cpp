// Material files are editable JSON (authored by hand, by agents and by the editor) and must reject invalid values
// without throwing. Whatever parses must also write and read back: the editor saves every material it loads.
#include "FuzzCommon.h"

#include <Basalt/Asset/MaterialAsset.h>
#include <Basalt/Core/JsonUtils.h>

#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	std::string error;
	const auto parsed = Basalt::ParseJson(BasaltFuzz::AsText(data, size), error);
	if (parsed.is_discarded())
		return 0;
	const auto material = Basalt::MaterialAsset::FromJson(parsed, error);
	if (!material)
		return 0;
	const auto written = Basalt::MaterialAsset::ToJson(*material, false, error);
	if (!written || !Basalt::MaterialAsset::FromJson(*written, error))
		std::abort();
	return 0;
}
