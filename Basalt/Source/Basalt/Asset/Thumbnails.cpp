#include "Basalt/Asset/Thumbnails.h"

#include "Basalt/Asset/ImageUtils.h"
#include "Basalt/Asset/TextureSource.h"
#include "Basalt/Core/FileSystem.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string_view>
#include <system_error>

namespace Basalt {

	namespace {

		std::string LowercaseExtension(const std::string& path)
		{
			std::string extension = std::filesystem::path(path).extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return extension;
		}

		// Fits width x height into maxSize x maxSize, keeping the aspect ratio and at least one pixel per side.
		void FitSize(uint32_t width, uint32_t height, uint32_t maxSize, uint32_t& outWidth, uint32_t& outHeight)
		{
			const double scale = std::min(1.0, static_cast<double>(maxSize) / static_cast<double>(std::max(width, height)));
			outWidth = std::max(1u, static_cast<uint32_t>(std::lround(width * scale)));
			outHeight = std::max(1u, static_cast<uint32_t>(std::lround(height * scale)));
		}

		// The source pixel range [begin, end) covered by target pixel `index` of `target` (at least one pixel).
		void Span(uint32_t index, uint32_t source, uint32_t target, uint32_t& outBegin, uint32_t& outEnd)
		{
			outBegin = static_cast<uint32_t>(static_cast<uint64_t>(index) * source / target);
			outEnd = std::max(outBegin + 1, static_cast<uint32_t>(static_cast<uint64_t>(index + 1) * source / target));
		}

		uint8_t EncodeSrgb(float linear)
		{
			const float c = std::clamp(linear, 0.0f, 1.0f);
			const float encoded = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
			return static_cast<uint8_t>(std::lround(encoded * 255.0f));
		}

		// Narkowicz's fit of the ACES filmic curve, the renderer's default look.
		float TonemapAces(float x)
		{
			return (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
		}

		uint64_t HashPath(const std::string& text)
		{
			uint64_t hash = 14695981039346656037ull;
			for (const char c : text)
			{
				hash ^= static_cast<unsigned char>(c);
				hash *= 1099511628211ull;
			}
			return hash;
		}

		// Separates the key from the details in a .key file (keys never contain it: their lines are a path and numbers).
		constexpr std::string_view DetailsSeparator = "\n--\n";

		std::string EntryName(const std::string& relativePath)
		{
			static constexpr char Digits[] = "0123456789abcdef";
			uint64_t hash = HashPath(relativePath);
			std::string name(16, '0');
			for (int i = 15; i >= 0; i--, hash >>= 4)
				name[static_cast<size_t>(i)] = Digits[hash & 0xF];
			return name;
		}

	}

	ThumbnailKind ClassifyThumbnail(const std::string& path)
	{
		const std::string extension = LowercaseExtension(path);
		if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga" || extension == ".bmp")
			return ThumbnailKind::Image;
		if (extension == ".hdr")
			return ThumbnailKind::Environment;
		if (extension == ".gltf" || extension == ".glb")
			return ThumbnailKind::Model;
		if (extension == ".bprefab")
			return ThumbnailKind::Prefab;
		return ThumbnailKind::None;
	}

	const char* ThumbnailKindName(ThumbnailKind kind)
	{
		switch (kind)
		{
			case ThumbnailKind::Image:
				return "Image";
			case ThumbnailKind::Environment:
				return "Environment";
			case ThumbnailKind::Model:
				return "Model";
			case ThumbnailKind::Prefab:
				return "Prefab";
			case ThumbnailKind::None:
				break;
		}
		return "None";
	}

	ThumbnailImage DownsampleImage(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, uint32_t targetWidth, uint32_t targetHeight)
	{
		ThumbnailImage image;
		if (width == 0 || height == 0 || rgba.size() < static_cast<size_t>(width) * height * 4)
			return image;
		image.Width = std::clamp(targetWidth, 1u, width);
		image.Height = std::clamp(targetHeight, 1u, height);
		image.Pixels.resize(static_cast<size_t>(image.Width) * image.Height * 4);
		for (uint32_t y = 0; y < image.Height; y++)
		{
			uint32_t y0 = 0, y1 = 0;
			Span(y, height, image.Height, y0, y1);
			for (uint32_t x = 0; x < image.Width; x++)
			{
				uint32_t x0 = 0, x1 = 0;
				Span(x, width, image.Width, x0, x1);
				uint32_t sum[4] = {};
				for (uint32_t sy = y0; sy < y1; sy++)
					for (uint32_t sx = x0; sx < x1; sx++)
						for (size_t c = 0; c < 4; c++)
							sum[c] += rgba[(static_cast<size_t>(sy) * width + sx) * 4 + c];
				const uint32_t count = (y1 - y0) * (x1 - x0);
				for (size_t c = 0; c < 4; c++)
					image.Pixels[(static_cast<size_t>(y) * image.Width + x) * 4 + c] = static_cast<uint8_t>((sum[c] + count / 2) / count);
			}
		}
		return image;
	}

	ThumbnailImage MakeImageThumbnail(const TextureSource& source, uint32_t maxSize)
	{
		uint32_t width = 0, height = 0;
		if (source.Width == 0 || source.Height == 0 || maxSize == 0)
			return {};
		FitSize(source.Width, source.Height, maxSize, width, height);
		if (source.Format == TextureFormat::RGBA8)
			return DownsampleImage(source.Pixels, source.Width, source.Height, width, height);

		// HDR: average linear radiance, then tonemap the average.
		if (source.Pixels.size() < static_cast<size_t>(source.Width) * source.Height * 16)
			return {};
		ThumbnailImage image;
		image.Width = width;
		image.Height = height;
		image.Pixels.resize(static_cast<size_t>(width) * height * 4);
		for (uint32_t y = 0; y < height; y++)
		{
			uint32_t y0 = 0, y1 = 0;
			Span(y, source.Height, height, y0, y1);
			for (uint32_t x = 0; x < width; x++)
			{
				uint32_t x0 = 0, x1 = 0;
				Span(x, source.Width, width, x0, x1);
				double sum[3] = {};
				for (uint32_t sy = y0; sy < y1; sy++)
					for (uint32_t sx = x0; sx < x1; sx++)
					{
						float texel[4];
						std::memcpy(texel, source.Pixels.data() + (static_cast<size_t>(sy) * source.Width + sx) * 16, sizeof(texel));
						for (size_t c = 0; c < 3; c++)
							sum[c] += std::isfinite(texel[c]) ? std::max(texel[c], 0.0f) : 0.0f;
					}
				const double count = static_cast<double>((y1 - y0) * (x1 - x0));
				uint8_t* pixel = image.Pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
				for (size_t c = 0; c < 3; c++)
					pixel[c] = EncodeSrgb(TonemapAces(static_cast<float>(sum[c] / count)));
				pixel[3] = 255;
			}
		}
		return image;
	}

	std::filesystem::path ThumbnailCache::DirectoryFor(const std::filesystem::path& projectDirectory)
	{
		return projectDirectory / "Cache" / "Thumbnails";
	}

	std::string ThumbnailCache::MakeKey(const std::filesystem::path& projectDirectory, const std::string& relativePath)
	{
		std::error_code error;
		const std::filesystem::path path = projectDirectory / relativePath;
		const auto size = std::filesystem::file_size(path, error);
		if (error)
			return {};
		const auto time = std::filesystem::last_write_time(path, error);
		if (error)
			return {};
		return relativePath + "\n" + std::to_string(size) + "\n" + std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count()) + "\n" + std::to_string(static_cast<int>(ClassifyThumbnail(relativePath))) + "\n" + std::to_string(Version);
	}

	std::filesystem::path ThumbnailCache::GetImagePath(const std::string& relativePath) const
	{
		return m_Directory / (EntryName(relativePath) + ".png");
	}

	std::optional<ThumbnailCache::Entry> ThumbnailCache::Load(const std::string& relativePath, const std::string& key) const
	{
		if (key.empty())
			return std::nullopt;
		const std::filesystem::path image = GetImagePath(relativePath);
		const auto stored = FileSystem::ReadTextFile(std::filesystem::path(image).replace_extension(".key"));
		// The key file holds the key, then the details after a separator line.
		if (!stored || stored->compare(0, key.size(), key) != 0 || (stored->size() > key.size() && stored->compare(key.size(), DetailsSeparator.size(), DetailsSeparator) != 0))
			return std::nullopt;
		std::string error;
		const Ref<TextureSource> texture = TextureSource::LoadFromFile(image, error);
		if (!texture || texture->Format != TextureFormat::RGBA8)
			return std::nullopt;
		Entry entry{ ThumbnailImage{ texture->Width, texture->Height, texture->Pixels }, {} };
		for (size_t start = key.size() + DetailsSeparator.size(); start < stored->size();)
		{
			const size_t end = std::min(stored->find('\n', start), stored->size());
			entry.Details.push_back(stored->substr(start, end - start));
			start = end + 1;
		}
		return entry;
	}

	bool ThumbnailCache::Store(const std::string& relativePath, const std::string& key, const Entry& entry, std::string& outError) const
	{
		const ThumbnailImage& image = entry.Image;
		std::error_code error;
		std::filesystem::create_directories(m_Directory, error);
		if (error)
		{
			outError = "cannot create '" + FileSystem::PathToUtf8(m_Directory) + "': " + error.message();
			return false;
		}
		const std::filesystem::path path = GetImagePath(relativePath);
		// The key goes last, so an interrupted store leaves an entry that no longer matches, never a wrong image.
		std::filesystem::remove(std::filesystem::path(path).replace_extension(".key"), error);
		if (!WritePng(path, image.Pixels, image.Width, image.Height, outError))
			return false;
		std::string text = key;
		if (!entry.Details.empty())
		{
			text += DetailsSeparator;
			for (size_t i = 0; i < entry.Details.size(); i++)
				text += (i ? "\n" : "") + entry.Details[i];
		}
		return FileSystem::WriteTextFile(std::filesystem::path(path).replace_extension(".key"), text, outError);
	}

	size_t ThumbnailCache::Prune(const std::filesystem::path& projectDirectory, bool everything) const
	{
		size_t removed = 0;
		std::error_code error;
		std::vector<std::filesystem::path> keys;
		for (auto it = std::filesystem::directory_iterator(m_Directory, error); !error && it != std::filesystem::directory_iterator(); it.increment(error))
			if (it->path().extension() == ".key")
				keys.push_back(it->path());
		for (const std::filesystem::path& keyPath : keys)
		{
			const auto key = FileSystem::ReadTextFile(keyPath);
			const std::string source = key ? key->substr(0, key->find('\n')) : std::string();
			if (!everything && !source.empty() && FileSystem::Exists(projectDirectory / source))
				continue;
			std::filesystem::remove(keyPath, error);
			std::filesystem::remove(std::filesystem::path(keyPath).replace_extension(".png"), error);
			removed++;
		}
		return removed;
	}

}
