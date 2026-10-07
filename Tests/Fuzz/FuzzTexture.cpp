// Image decoding (PNG, JPEG, TGA, BMP, HDR) from memory, as used for textures and embedded glTF images.
#include "FuzzCommon.h"

#include <Basalt/Asset/TextureSource.h>

#include <stb_image.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	if (size == 0 || size > static_cast<size_t>(INT32_MAX))
		return 0;

	// A few header bytes can describe a legitimately large image (the engine accepts up to 16384 px per
	// side), and decoding one under ASan takes seconds without exercising any more parser logic. Skip those
	// so the fuzzer spends its time on the decoders, as image-library fuzz harnesses usually do.
	constexpr int64_t MaxFuzzPixels = int64_t{ 1 } << 22;
	int width = 0;
	int height = 0;
	int channels = 0;
	if (stbi_info_from_memory(data, static_cast<int>(size), &width, &height, &channels) && int64_t{ width } * height > MaxFuzzPixels)
		return 0;

	std::string error;
	Basalt::TextureSource::LoadFromMemory(data, size, error);
	return 0;
}
