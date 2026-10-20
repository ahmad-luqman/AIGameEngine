#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Core/MemoryTracker.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Basalt {

	enum class TextureFormat
	{
		RGBA8 = 0,
		// 32-bit float RGBA, used for HDR environment maps.
		RGBA32F,
		BC7
	};

	// CPU image pixels (top row first) or BC7 mip blocks. The renderer creates GPU textures.
	class TextureSource
	{
	public:
		uint32_t Width = 0;
		uint32_t Height = 0;
		TextureFormat Format = TextureFormat::RGBA8;
		std::vector<uint8_t> Pixels;
		std::string Key;
		// BC7 levels are concatenated in Pixels; offsets include the base level.
		std::vector<size_t> MipOffsets;
		// Authoring intent; a material slot determines the GPU sampling format.
		bool SRGB = false;

		// Refresh after mutating payload vectors; asset loads do this before publishing.
		void UpdateMemoryUsage();

	private:
		MemoryAllocation m_Memory;

	public:
		size_t GetBytesPerPixel() const { return Format == TextureFormat::BC7 ? 0 : (Format == TextureFormat::RGBA8 ? 4 : 16); }

		// PNG/JPEG/TGA/BMP (RGBA8), Radiance HDR (RGBA32F), KTX2 (BC7). Reports failures through outError.
		static Ref<TextureSource> LoadFromFile(const std::filesystem::path& path, std::string& outError);
		static Ref<TextureSource> LoadFromMemory(const uint8_t* data, size_t size, std::string& outError);
		// Returns an RGBA8 copy of BC7 level zero for CPU consumers and GPU fallback.
		bool HasValidBC7Layout() const;
		Ref<TextureSource> DecodeRGBA8(std::string& outError, uint32_t mip = 0) const;
		static Ref<TextureSource> CreateSolid(uint32_t rgba, const std::string& key);
	};

}
