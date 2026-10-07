#pragma once

#include "Basalt/Core/Base.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Basalt {

	enum class TextureFormat
	{
		RGBA8 = 0,
		// 32-bit float RGBA, used for HDR environment maps.
		RGBA32F
	};

	// Decoded pixels of an image asset (top row first). GPU textures are created by the renderer.
	class TextureSource
	{
	public:
		uint32_t Width = 0;
		uint32_t Height = 0;
		TextureFormat Format = TextureFormat::RGBA8;
		std::vector<uint8_t> Pixels;
		std::string Key;

		size_t GetBytesPerPixel() const { return Format == TextureFormat::RGBA8 ? 4 : 16; }

		// PNG, JPEG, TGA, BMP (RGBA8) and Radiance .hdr (RGBA32F). Returns nullptr and sets outError on failure.
		static Ref<TextureSource> LoadFromFile(const std::filesystem::path& path, std::string& outError);
		static Ref<TextureSource> LoadFromMemory(const uint8_t* data, size_t size, std::string& outError);
		static Ref<TextureSource> CreateSolid(uint32_t rgba, const std::string& key);
	};

}
