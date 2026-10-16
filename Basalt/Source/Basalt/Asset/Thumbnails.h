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

		// Non-empty, with exactly Width x Height pixels.
		bool IsValid() const { return Width > 0 && Height > 0 && Pixels.size() == static_cast<size_t>(Width) * Height * 4; }
	};

	// Scales RGBA8 pixels down by averaging the source pixels each target pixel covers (a box filter, so thin
	// details fade instead of flickering). Larger targets are not upscaled. Empty for invalid input.
	ThumbnailImage DownsampleImage(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, uint32_t targetWidth, uint32_t targetHeight);

	// Fits an image into maxSize x maxSize keeping its aspect ratio (never upscaled). HDR (RGBA32F) sources are
	// averaged in linear light, then tonemapped (ACES fit) and sRGB-encoded; non-finite texels count as black.
	// Empty for invalid input.
	ThumbnailImage MakeImageThumbnail(const TextureSource& source, uint32_t maxSize);

	// A project's previews on disk, under <project>/Cache/Thumbnails: outside Assets/, so never exported. Each
	// entry is <hash of the project-relative path>.png plus a .key file holding the version key (path, size,
	// modification time, kind and Version, and the same for each dependency; not a content hash) and the tooltip
	// details. A changed source or dependency no longer matches its key and is made again. Headless.
	class ThumbnailCache
	{
	public:
		// Bumped when previews change their look, so old caches are rebuilt.
		static constexpr uint32_t Version = 1;

		struct Entry
		{
			ThumbnailImage Image;
			// Lines describing the source for tooltips ("1024 x 512", "12 meshes", ...).
			std::vector<std::string> Details;
		};

		explicit ThumbnailCache(std::filesystem::path projectDirectory);

		static std::filesystem::path DirectoryFor(const std::filesystem::path& projectDirectory);
		// The version key of a project-relative source file, or nullopt (with the reason) when it cannot be read. It
		// covers the files the preview also reads (GetDependencies), so changing one of those makes it again.
		std::optional<std::string> MakeKey(const std::string& relativePath, std::string& outError) const;
		// The other files a preview reads: a glTF's external buffers and images; a prefab's models (and theirs),
		// textures and fonts. Project-relative, sorted.
		std::vector<std::string> GetDependencies(const std::string& relativePath) const;

		// The cached preview when it was made from the same version of the source (key).
		std::optional<Entry> Load(const std::string& relativePath, const std::string& key) const;
		bool Store(const std::string& relativePath, const std::string& key, const Entry& entry, std::string& outError) const;
		std::filesystem::path GetImagePath(const std::string& relativePath) const;
		// Deletes the entries whose source file is gone (and images left without a key by an interrupted store).
		// Returns how many files it removed; failures are logged.
		size_t PruneMissing() const;
		// Deletes every entry. Returns how many files it removed; failures are logged.
		size_t Clear() const;

		const std::filesystem::path& GetDirectory() const { return m_Directory; }

	private:
		size_t Remove(bool everything) const;

		std::filesystem::path m_ProjectDirectory;
		std::filesystem::path m_Directory;
	};

}
