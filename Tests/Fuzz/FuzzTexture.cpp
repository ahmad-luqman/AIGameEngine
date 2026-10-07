// Image decoding (PNG, JPEG, TGA, BMP, HDR) from memory, as used for textures and embedded glTF images.
#include "FuzzCommon.h"

#include <Basalt/Asset/TextureSource.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	std::string error;
	Basalt::TextureSource::LoadFromMemory(data, size, error);
	return 0;
}
