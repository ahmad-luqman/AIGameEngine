#pragma once

#include "Basalt/Asset/TextureSource.h"

namespace Basalt {

	// Headless KTX2 import/transcoding and deterministic BC7 authoring. Sources are never modified.
	class TextureCompression
	{
	public:
		static bool IsKTX2(const uint8_t* data, size_t size);
		static Ref<TextureSource> LoadKTX2(const uint8_t* data, size_t size, std::string& outError);
		static bool EncodeKTX2(const TextureSource& source, bool srgb, std::vector<uint8_t>& out, std::string& outError);
		static bool Import(const std::filesystem::path& source, const std::filesystem::path& output, const std::filesystem::path& cacheDirectory, bool srgb, bool& cacheHit, std::string& outError);
	};

}
