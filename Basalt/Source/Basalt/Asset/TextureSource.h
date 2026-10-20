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
		// Texture limits shared by KTX2 loading, BC7 authoring and layout validation.
		static constexpr uint32_t MaxDimension = 16384;
		static constexpr uint32_t MaxMipLevels = 15; // A full chain of MaxDimension.

		// Levels in a full mip chain down to 1x1.
		static uint32_t MipCount(uint32_t width, uint32_t height);
		// Bytes of BC7 level `mip` (4x4 blocks of 16 bytes) of a width x height base level.
		static size_t BC7LevelSize(uint32_t width, uint32_t height, uint32_t mip);

		// PNG/JPEG/TGA/BMP (RGBA8), Radiance HDR (RGBA32F), KTX2 (BC7). Reports failures through outError.
		static Ref<TextureSource> LoadFromFile(const std::filesystem::path& path, std::string& outError);
		static Ref<TextureSource> LoadFromMemory(const uint8_t* data, size_t size, std::string& outError);
		// True for a BC7 source whose MipOffsets form a contiguous chain from 0 (no level past 1x1, at most
		// MaxDimension per side) that exactly covers Pixels.
		bool HasValidBC7Layout() const;
		// RGBA8 copy of one BC7 level, for CPU consumers (thumbnails) and GPUs that cannot sample BC7.
		Ref<TextureSource> DecodeRGBA8(uint32_t mip, std::string& outError) const;
		static Ref<TextureSource> CreateSolid(uint32_t rgba, const std::string& key);
	};

}
