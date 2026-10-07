#include "Basalt/Asset/TextureSource.h"

#include "Basalt/Core/FileSystem.h"

#include <stb_image.h>

#include <cstring>

namespace Basalt {

	Ref<TextureSource> TextureSource::LoadFromMemory(const uint8_t* data, size_t size, std::string& outError)
	{
		if (!data || size == 0 || size > static_cast<size_t>(INT32_MAX))
		{
			outError = "empty or oversized image data";
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
			texture->Format = TextureFormat::RGBA8;
			texture->Pixels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
			stbi_image_free(pixels);
		}

		texture->Width = static_cast<uint32_t>(width);
		texture->Height = static_cast<uint32_t>(height);
		return texture;
	}

	Ref<TextureSource> TextureSource::LoadFromFile(const std::filesystem::path& path, std::string& outError)
	{
		const auto bytes = FileSystem::ReadBinaryFile(path);
		if (!bytes)
		{
			outError = "cannot read '" + path.string() + "'";
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
		return texture;
	}

}
