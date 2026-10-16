#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	class TextureSource;

	// What a content-browser preview of a file shows.
	enum class ThumbnailKind : uint8_t
	{
		// No preview (scripts, scenes, audio, folders, ...).
		None,
		// The picture itself, scaled down (PNG, JPEG, TGA, BMP).
		Image,
		// An HDR environment, tonemapped and scaled down.
		Environment,
		// A glTF model rendered in a studio setup (needs a GPU).
		Model,
		// A prefab rendered in a studio setup (needs a GPU).
		Prefab
	};

	ThumbnailKind ClassifyThumbnail(const std::string& path);
	// "None", "Image", "Environment", "Model" or "Prefab".
	const char* ThumbnailKindName(ThumbnailKind kind);
	// Image and Environment previews are made on the CPU (headless); the others are rendered.
	inline bool IsRenderedThumbnail(ThumbnailKind kind)
	{
		return kind == ThumbnailKind::Model || kind == ThumbnailKind::Prefab;
	}

	// An RGBA8 preview image, top row first.
	struct ThumbnailImage
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<uint8_t> Pixels;
	};

	// Scales RGBA8 pixels down by averaging the source pixels each target pixel covers (a box filter, so thin
	// details fade instead of flickering). Larger targets are not upscaled.
	ThumbnailImage DownsampleImage(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, uint32_t targetWidth, uint32_t targetHeight);

	// Fits an image into maxSize x maxSize keeping its aspect ratio (never upscaled). HDR (RGBA32F) sources are
	// averaged in linear light, then tonemapped (ACES fit) and sRGB-encoded; non-finite texels count as black.
	ThumbnailImage MakeImageThumbnail(const TextureSource& source, uint32_t maxSize);

	// Previews kept on disk under <project>/Cache/Thumbnails, outside Assets/ so they are never exported. Each
	// entry is <hash of the project-relative path>.png plus a .key file recording which version of the source
	// it shows (path, size, modification time, preview version): a changed source no longer matches and is
	// rendered again. Headless.
	class ThumbnailCache
	{
	public:
		// Bumped when previews change their look, so old caches are rebuilt.
		static constexpr uint32_t Version = 1;

		explicit ThumbnailCache(std::filesystem::path directory)
			: m_Directory(std::move(directory))
		{
		}

		static std::filesystem::path DirectoryFor(const std::filesystem::path& projectDirectory);
		// Identifies the source file's current contents; empty when the file does not exist.
		static std::string MakeKey(const std::filesystem::path& projectDirectory, const std::string& relativePath);

		struct Entry
		{
			ThumbnailImage Image;
			// Lines describing the source for tooltips ("1024 x 512", "12 meshes", ...).
			std::vector<std::string> Details;
		};

		// The cached preview when it was made from the same version of the source (key).
		std::optional<Entry> Load(const std::string& relativePath, const std::string& key) const;
		bool Store(const std::string& relativePath, const std::string& key, const Entry& entry, std::string& outError) const;
		std::filesystem::path GetImagePath(const std::string& relativePath) const;
		// Deletes entries whose source file is gone, or every entry with `everything`. Returns how many it removed.
		size_t Prune(const std::filesystem::path& projectDirectory, bool everything) const;

		const std::filesystem::path& GetDirectory() const { return m_Directory; }

	private:
		std::filesystem::path m_Directory;
	};

}
