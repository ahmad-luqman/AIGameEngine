#include "Basalt/Asset/TextureSource.h"

#include "Basalt/Asset/TextureCompression.h"
#include "Basalt/Core/FileSystem.h"

#include <bc7decomp.h>
#include <spdlog/fmt/fmt.h>
#include <stb_image.h>

#include <algorithm>
#include <cstring>

namespace Basalt {

	uint32_t TextureSource::MipCount(uint32_t width, uint32_t height)
	{
		uint32_t count = 1;
		for (uint32_t size = std::max(width, height); size > 1; size /= 2)
			count++;
		return count;
	}

	size_t TextureSource::BC7LevelSize(uint32_t width, uint32_t height, uint32_t mip)
	{
		const uint32_t w = std::max(1u, width >> mip), h = std::max(1u, height >> mip);
		return size_t((w + 3) / 4) * ((h + 3) / 4) * 16;
	}

	bool TextureSource::HasValidBC7Layout() const
	{
		if (Format != TextureFormat::BC7 || !Width || !Height || Width > MaxDimension || Height > MaxDimension || MipOffsets.empty() || MipOffsets.size() > MipCount(Width, Height))
			return false;
		size_t expected = 0;
		for (size_t mip = 0; mip < MipOffsets.size(); mip++)
		{
			if (MipOffsets[mip] != expected)
				return false;
			expected += BC7LevelSize(Width, Height, static_cast<uint32_t>(mip));
		}
		return expected == Pixels.size();
	}

	Ref<TextureSource> TextureSource::DecodeRGBA8(uint32_t mip, std::string& outError) const
	{
		if (!HasValidBC7Layout())
		{
			outError = "invalid BC7 mip layout";
			return nullptr;
		}
		if (mip >= MipOffsets.size())
		{
			outError = fmt::format("BC7 mip {} out of range ({} levels)", mip, MipOffsets.size());
			return nullptr;
		}
		const uint32_t width = std::max(1u, Width >> mip), height = std::max(1u, Height >> mip);
		const uint32_t blocksPerRow = (width + 3) / 4;

		auto result = CreateRef<TextureSource>();
		result->Width = width;
		result->Height = height;
		result->SRGB = SRGB;
		result->Pixels.resize(size_t(width) * height * 4);
		for (uint32_t by = 0; by < (height + 3) / 4; by++)
			for (uint32_t bx = 0; bx < blocksPerRow; bx++)
			{
				bc7decomp::color_rgba block[16];
				if (!bc7decomp::unpack_bc7(Pixels.data() + MipOffsets[mip] + (size_t(by) * blocksPerRow + bx) * 16, block))
				{
					outError = fmt::format("invalid BC7 block in mip {}", mip);
					return nullptr;
				}
				for (uint32_t y = 0; y < 4 && by * 4 + y < height; y++)
					for (uint32_t x = 0; x < 4 && bx * 4 + x < width; x++)
						std::memcpy(result->Pixels.data() + (size_t(by * 4 + y) * width + size_t(bx) * 4 + x) * 4, block[y * 4 + x].m_comps, 4);
			}
		result->UpdateMemoryUsage();
		outError.clear();
		return result;
	}

	void TextureSource::UpdateMemoryUsage()
	{
		m_Memory.Resize(sizeof(TextureSource) + Pixels.capacity() + MipOffsets.capacity() * sizeof(size_t));
	}

	namespace {

		// stb_image computes abs() of a BMP's signed 32-bit height, which overflows (undefined behaviour) for
		// INT32_MIN; found by FuzzTexture and still present upstream. No valid image has that height.
		bool HasInvalidBmpHeight(const uint8_t* data, size_t size)
		{
			if (size < 26 || data[0] != 'B' || data[1] != 'M')
				return false;
			const uint32_t headerSize = static_cast<uint32_t>(data[14]) | (static_cast<uint32_t>(data[15]) << 8) | (static_cast<uint32_t>(data[16]) << 16) | (static_cast<uint32_t>(data[17]) << 24);
			if (headerSize == 12) // BITMAPCOREHEADER: 16-bit dimensions, no overflow possible
				return false;
			const uint32_t height = static_cast<uint32_t>(data[22]) | (static_cast<uint32_t>(data[23]) << 8) | (static_cast<uint32_t>(data[24]) << 16) | (static_cast<uint32_t>(data[25]) << 24);
			return height == 0x80000000u;
		}

	}

	Ref<TextureSource> TextureSource::LoadFromMemory(const uint8_t* data, size_t size, std::string& outError)
	{
		if (!data || size == 0 || size > static_cast<size_t>(INT32_MAX))
		{
			outError = "empty or oversized image data";
			return nullptr;
		}

		if (TextureCompression::IsKTX2(data, size))
			return TextureCompression::LoadKTX2(data, size, outError);

		if (HasInvalidBmpHeight(data, size))
		{
			outError = "invalid BMP height";
			return nullptr;
		}

		const int length = static_cast<int>(size);
		int width = 0;
		int height = 0;
		int channels = 0;
		Ref<TextureSource> texture = CreateRef<TextureSource>();

		if (stbi_is_hdr_from_memory(data, length))
		{
			float* pixels = stbi_loadf_from_memory(data, length, &width, &height, &channels, 4);
			if (!pixels)
			{
				outError = stbi_failure_reason() ? stbi_failure_reason() : "unknown decode error";
				return nullptr;
			}
			if (width <= 0 || height <= 0)
			{
				// stb_image accepts a Radiance header with a zero size and still returns a buffer.
				stbi_image_free(pixels);
				outError = "image has no pixels";
				return nullptr;
			}
			texture->Format = TextureFormat::RGBA32F;
			texture->Pixels.resize(static_cast<size_t>(width) * height * 16);
			std::memcpy(texture->Pixels.data(), pixels, texture->Pixels.size());
			stbi_image_free(pixels);
		}
		else
		{
			stbi_uc* pixels = stbi_load_from_memory(data, length, &width, &height, &channels, 4);
			if (!pixels)
			{
				outError = stbi_failure_reason() ? stbi_failure_reason() : "unknown decode error";
				return nullptr;
			}
			if (width <= 0 || height <= 0)
			{
				stbi_image_free(pixels);
				outError = "image has no pixels";
				return nullptr;
			}
			texture->Format = TextureFormat::RGBA8;
			texture->Pixels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
			stbi_image_free(pixels);
		}

		texture->Width = static_cast<uint32_t>(width);
		texture->Height = static_cast<uint32_t>(height);
		texture->UpdateMemoryUsage();
		return texture;
	}

	Ref<TextureSource> TextureSource::LoadFromFile(const std::filesystem::path& path, std::string& outError)
	{
		std::string readError;
		const auto bytes = FileSystem::ReadBinaryFile(path, &readError);
		if (!bytes)
		{
			outError = "cannot read '" + FileSystem::PathToUtf8(path) + "'" + (readError.empty() ? std::string() : " (" + readError + ")");
			return nullptr;
		}
		Ref<TextureSource> texture = LoadFromMemory(bytes->data(), bytes->size(), outError);
		if (!texture)
			outError = path.filename().string() + ": " + outError;
		return texture;
	}

	Ref<TextureSource> TextureSource::CreateSolid(uint32_t rgba, const std::string& key)
	{
		Ref<TextureSource> texture = CreateRef<TextureSource>();
		texture->Width = 1;
		texture->Height = 1;
		texture->Format = TextureFormat::RGBA8;
		texture->Pixels = { static_cast<uint8_t>(rgba >> 24), static_cast<uint8_t>(rgba >> 16), static_cast<uint8_t>(rgba >> 8), static_cast<uint8_t>(rgba) };
		texture->Key = key;
		texture->UpdateMemoryUsage();
		return texture;
	}

}
