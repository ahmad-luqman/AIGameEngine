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
		struct ImportResult
		{
			bool CacheHit = false;
			// Non-fatal cache problems: the output is still written, only re-encoding is not avoided.
			std::vector<std::string> Warnings;
		};
		// Compresses `source` (8-bit PNG/JPEG/TGA/BMP) into the separate KTX2 `output`, reusing a content-keyed
		// encode from `cacheDirectory` (empty: no cache). Fails only when the output cannot be produced.
		static bool Import(const std::filesystem::path& source, const std::filesystem::path& output, const std::filesystem::path& cacheDirectory, bool srgb, ImportResult& result, std::string& outError);
	};

}
