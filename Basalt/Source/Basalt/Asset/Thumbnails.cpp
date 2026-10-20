#include "Basalt/Asset/AssetRegistry.h"

#include "Basalt/Asset/AssetKey.h"
#include "Basalt/Asset/ImageUtils.h"
#include "Basalt/Asset/MaterialAsset.h"
#include "Basalt/Asset/MeshImporter.h"
#include "Basalt/Asset/TextureSource.h"
#include "Basalt/Asset/Thumbnails.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Hash.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Project/Project.h"
#include "Basalt/Scene/ComponentRegistry.h"
#include "Basalt/Scene/Components.h"

#include <cgltf.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <set>
#include <string_view>
#include <system_error>

namespace Basalt {

	std::string MaterialPreviewKey(const MaterialComponent& m)
	{
		// Grouped like MaterialComponent; Tests check that every serialized field changes the key.
		const nlohmann::json key = nlohmann::json::array({ m.Material, m.AlbedoColor.r, m.AlbedoColor.g, m.AlbedoColor.b, m.AlbedoColor.a, m.Metallic, m.Roughness, m.EmissiveColor.r, m.EmissiveColor.g, m.EmissiveColor.b, m.EmissiveIntensity,
														   m.AlbedoMap, m.NormalMap, m.MetallicRoughnessMap, m.OcclusionMap, m.EmissiveMap,
														   m.Clearcoat, m.ClearcoatRoughness, m.Transmission, m.ClearcoatMap, m.ClearcoatRoughnessMap, m.ClearcoatNormalMap, m.TransmissionMap,
														   m.UVOffset.x, m.UVOffset.y, m.UVScale.x, m.UVScale.y, m.UVRotation });
		return key.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
	}

	namespace {

		// The registry that decodes a project's UUID references: the active project's when it is this one, else a fresh
		// scan of `local` (a registry of projectDirectory; null, with a warning, when that fails).
		const AssetRegistry* RegistryFor(const std::filesystem::path& projectDirectory, AssetRegistry& local)
		{
			std::error_code pathError;
			if (const auto& active = Project::GetActive(); active && std::filesystem::equivalent(active->GetDirectory(), projectDirectory, pathError))
				return &active->GetAssets();
			std::string error;
			if (local.Refresh(error))
				return &local;
			BS_CORE_WARN("Thumbnail dependencies: {}", error);
			return nullptr;
		}

		// The path an asset reference names: a string as is, a {UUID, Path} object through the registry (empty when it
		// does not resolve; the preview loader reports that, dependency polling must not log every frame).
		std::string DecodeReference(const nlohmann::json& value, const AssetRegistry* assets)
		{
			std::string key, error;
			if (value.is_string())
				key = value.get<std::string>();
			else if (assets && !assets->Decode(value, key, error))
				key.clear();
			return key;
		}

		void AddModelDependencies(const std::filesystem::path& projectDirectory, const std::string& model, std::set<std::string>& outDependencies);

		// The files a material file's textures load from (a model for its embedded images, with that model's files).
		void AddMaterialDependencies(const std::filesystem::path& projectDirectory, const AssetRegistry* assets, const std::string& material, std::set<std::string>& outDependencies)
		{
			const auto text = FileSystem::ReadTextFile(projectDirectory / FileSystem::PathFromUtf8(material));
			const nlohmann::json data = text ? ParseJson(*text) : nlohmann::json();
			if (!data.is_object())
				return;
			for (const char* field : MaterialAsset::MapFields)
				if (const auto value = data.find(field); value != data.end())
					if (const std::string source(GetAssetSource(DecodeReference(*value, assets))); source.starts_with("Assets/"))
					{
						outDependencies.insert(source);
						if (ClassifyThumbnail(source) == ThumbnailKind::Model)
							AddModelDependencies(projectDirectory, source, outDependencies);
					}
		}

	}

	std::vector<std::string> MaterialPreviewDependencies(const MaterialComponent& m)
	{
		std::set<std::string> dependencies;
		for (const std::string* map : { &m.AlbedoMap, &m.NormalMap, &m.MetallicRoughnessMap, &m.OcclusionMap, &m.EmissiveMap, &m.ClearcoatMap, &m.ClearcoatRoughnessMap, &m.ClearcoatNormalMap, &m.TransmissionMap, &m.Material })
			if (!map->empty())
				dependencies.insert(std::string(GetAssetSource(*map)));
		if (const auto& project = Project::GetActive(); project && MaterialAsset::IsMaterialFile(m.Material))
			AddMaterialDependencies(project->GetDirectory(), &project->GetAssets(), m.Material, dependencies);
		return { dependencies.begin(), dependencies.end() };
	}

	namespace {

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

		// A .key file: the version key, then (after a separator line) one tooltip detail per line. Keys never
		// contain the separator (their lines are a path and numbers); details have newlines replaced.
		constexpr std::string_view DetailsSeparator = "\n--\n";

		struct KeyFile
		{
			std::string Key;
			std::vector<std::string> Details;
		};

		std::string FormatKeyFile(const std::string& key, const std::vector<std::string>& details)
		{
			std::string text = key;
			if (!details.empty())
				text += DetailsSeparator;
			for (size_t i = 0; i < details.size(); i++)
			{
				std::string line = details[i];
				std::replace(line.begin(), line.end(), '\n', ' ');
				text += (i ? "\n" : "") + line;
			}
			return text;
		}

		std::optional<KeyFile> ParseKeyFile(const std::string& text)
		{
			const size_t separator = text.find(DetailsSeparator);
			KeyFile file{ text.substr(0, separator), {} };
			if (file.Key.empty())
				return std::nullopt;
			const size_t detailsStart = separator == std::string::npos ? text.size() : separator + DetailsSeparator.size();
			for (size_t start = detailsStart; start < text.size();)
			{
				const size_t end = std::min(text.find('\n', start), text.size());
				file.Details.push_back(text.substr(start, end - start));
				start = end + 1;
			}
			return file;
		}

		// The project-relative source path: a key's first line (MakeKey).
		std::string SourceOfKey(const std::string& key)
		{
			return key.substr(0, key.find('\n'));
		}

		std::filesystem::path KeyPathOf(const std::filesystem::path& image)
		{
			return std::filesystem::path(image).replace_extension(".key");
		}

		// "<size>\n<modification time>" of a file, or nullopt (with the error).
		std::optional<std::string> FileStamp(const std::filesystem::path& path, std::error_code& outError)
		{
			const auto size = std::filesystem::file_size(path, outError);
			if (outError)
				return std::nullopt;
			const auto time = std::filesystem::last_write_time(path, outError);
			if (outError)
				return std::nullopt;
			return std::to_string(size) + "\n" + std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());
		}

		// A glTF's external buffers and images (not data: URIs), project-relative. The JSON is parsed, the buffers are
		// not loaded.
		void AddModelDependencies(const std::filesystem::path& projectDirectory, const std::string& model, std::set<std::string>& outDependencies)
		{
			cgltf_options options = {};
			cgltf_data* data = nullptr;
			const std::string path = FileSystem::PathToUtf8(projectDirectory / FileSystem::PathFromUtf8(model));
			if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success)
				return;
			const auto add = [&](const char* uri) {
				if (auto file = MeshImporter::GetExternalFileKey(model, uri))
					outDependencies.insert(std::move(*file));
			};
			for (cgltf_size i = 0; i < data->buffers_count; i++)
				add(data->buffers[i].uri);
			for (cgltf_size i = 0; i < data->images_count; i++)
				add(data->images[i].uri);
			cgltf_free(data);
		}

	}

	ThumbnailKind ClassifyThumbnail(const std::string& path)
	{
		const std::string extension = FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(path));
		if (extension == ".ktx2" || extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga" || extension == ".bmp")
			return ThumbnailKind::Image;
		if (extension == ".hdr")
			return ThumbnailKind::Environment;
		if (extension == ".gltf" || extension == ".glb")
			return ThumbnailKind::Model;
		if (extension == ".bprefab")
			return ThumbnailKind::Prefab;
		if (extension == MaterialAsset::Extension)
			return ThumbnailKind::Material;
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
			case ThumbnailKind::Material:
				return "Material";
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
				uint64_t sum[4] = {};
				for (uint32_t sy = y0; sy < y1; sy++)
					for (uint32_t sx = x0; sx < x1; sx++)
						for (size_t c = 0; c < 4; c++)
							sum[c] += rgba[(static_cast<size_t>(sy) * width + sx) * 4 + c];
				const uint64_t count = static_cast<uint64_t>(y1 - y0) * (x1 - x0);
				for (size_t c = 0; c < 4; c++)
					image.Pixels[(static_cast<size_t>(y) * image.Width + x) * 4 + c] = static_cast<uint8_t>((sum[c] + count / 2) / count);
			}
		}
		return image;
	}

	ThumbnailImage MakeImageThumbnail(const TextureSource& source, uint32_t maxSize, std::string* outError)
	{
		uint32_t width = 0, height = 0;
		if (source.Width == 0 || source.Height == 0 || maxSize == 0)
			return {};
		if (source.Format == TextureFormat::BC7)
		{
			std::string error;
			auto decoded = source.DecodeRGBA8(0, error);
			if (!decoded)
			{
				// A caller that takes the reason reports it with its own context.
				if (outError)
					*outError = error;
				else
					BS_CORE_ERROR("BC7 thumbnail '{}': {}", source.Key, error);
				return {};
			}
			return MakeImageThumbnail(*decoded, maxSize, outError);
		}
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

	ThumbnailCache::ThumbnailCache(std::filesystem::path projectDirectory)
		: m_ProjectDirectory(std::move(projectDirectory))
		, m_Directory(DirectoryFor(m_ProjectDirectory))
	{
	}

	std::filesystem::path ThumbnailCache::DirectoryFor(const std::filesystem::path& projectDirectory)
	{
		return projectDirectory / "Cache" / "Thumbnails";
	}

	std::optional<std::string> ThumbnailCache::MakeKey(const std::string& relativePath, std::string& outError) const
	{
		std::error_code error;
		const std::optional<std::string> stamp = FileStamp(m_ProjectDirectory / FileSystem::PathFromUtf8(relativePath), error);
		if (!stamp)
		{
			outError = error == std::errc::no_such_file_or_directory ? "the file is missing" : "cannot read the file: " + error.message();
			return std::nullopt;
		}
		std::string key = relativePath + "\n" + *stamp + "\n" + std::to_string(static_cast<int>(ClassifyThumbnail(relativePath))) + "\n" + std::to_string(Version);
		// A missing dependency is part of the version too: the preview made without it differs.
		for (const std::string& dependency : GetDependencies(relativePath))
		{
			std::error_code dependencyError;
			key += "\n" + dependency + " " + FileStamp(m_ProjectDirectory / FileSystem::PathFromUtf8(dependency), dependencyError).value_or("missing");
		}
		return key;
	}

	std::vector<std::string> ThumbnailCache::GetDependencies(const std::string& relativePath) const
	{
		std::set<std::string> dependencies;
		switch (ClassifyThumbnail(relativePath))
		{
			case ThumbnailKind::Model:
				AddModelDependencies(m_ProjectDirectory, relativePath, dependencies);
				break;
			case ThumbnailKind::Prefab:
			{
				const auto text = FileSystem::ReadTextFile(m_ProjectDirectory / FileSystem::PathFromUtf8(relativePath));
				const nlohmann::json data = text ? ParseJson(*text) : nlohmann::json();
				const auto entities = data.is_object() ? data.find("Entities") : data.end();
				if (entities == data.end() || !entities->is_array())
					break;
				AssetRegistry local(m_ProjectDirectory);
				const AssetRegistry* assets = RegistryFor(m_ProjectDirectory, local);
				for (const auto& entity : *entities)
				{
					const auto components = entity.is_object() ? entity.find("Components") : entity.end();
					if (components == entity.end() || !components->is_object())
						continue;
					for (const auto& [component, fields] : components->items())
						if (fields.is_object())
							for (const auto& [field, value] : fields.items())
							{
								std::string key;
								if (value.is_string())
									key = value.get<std::string>();
								else if (const auto* info = ComponentRegistry::Find(component); info && std::find(info->AssetFields.begin(), info->AssetFields.end(), field) != info->AssetFields.end())
									key = DecodeReference(value, assets);
								if (key.starts_with("Assets/"))
								{
									const std::string file(GetAssetSource(key));
									dependencies.insert(file);
									if (ClassifyThumbnail(file) == ThumbnailKind::Model)
										AddModelDependencies(m_ProjectDirectory, file, dependencies);
									else if (ClassifyThumbnail(file) == ThumbnailKind::Material)
										AddMaterialDependencies(m_ProjectDirectory, assets, file, dependencies);
								}
							}
				}
				break;
			}
			case ThumbnailKind::Material:
			{
				AssetRegistry local(m_ProjectDirectory);
				AddMaterialDependencies(m_ProjectDirectory, RegistryFor(m_ProjectDirectory, local), relativePath, dependencies);
				break;
			}
			case ThumbnailKind::Image:
			case ThumbnailKind::Environment:
			case ThumbnailKind::None:
				break;
		}
		return { dependencies.begin(), dependencies.end() };
	}

	std::filesystem::path ThumbnailCache::GetImagePath(const std::string& relativePath) const
	{
		return m_Directory / (ToHex64(HashFnv1a64(relativePath)) + ".png");
	}

	std::optional<ThumbnailCache::Entry> ThumbnailCache::Load(const std::string& relativePath, const std::string& key) const
	{
		if (key.empty())
			return std::nullopt;
		const std::filesystem::path image = GetImagePath(relativePath);
		const auto stored = FileSystem::ReadTextFile(KeyPathOf(image));
		const auto keyFile = stored ? ParseKeyFile(*stored) : std::nullopt;
		if (!keyFile || keyFile->Key != key)
			return std::nullopt;
		std::string error;
		const Ref<TextureSource> texture = TextureSource::LoadFromFile(image, error);
		if (!texture || texture->Format != TextureFormat::RGBA8)
		{
			// The preview is made again (and the entry replaced), so this costs one render, not a missing preview.
			BS_CORE_WARN("Thumbnails: the cached preview of '{}' is unreadable ({}); making it again", relativePath, texture ? "not RGBA8" : error);
			return std::nullopt;
		}
		Entry entry{ ThumbnailImage{ texture->Width, texture->Height, texture->Pixels }, keyFile->Details };
		if (!entry.Image.IsValid())
			return std::nullopt;
		return entry;
	}

	bool ThumbnailCache::Store(const std::string& relativePath, const std::string& key, const Entry& entry, std::string& outError) const
	{
		if (!entry.Image.IsValid())
		{
			outError = "the preview is empty";
			return false;
		}
		std::error_code error;
		std::filesystem::create_directories(m_Directory, error);
		if (error)
		{
			outError = "cannot create '" + FileSystem::PathToUtf8(m_Directory) + "': " + error.message();
			return false;
		}
		const std::filesystem::path path = GetImagePath(relativePath);
		const std::filesystem::path keyPath = KeyPathOf(path);
		// The key goes last, so an interrupted store leaves an entry that no longer matches, never a wrong image.
		std::filesystem::remove(keyPath, error);
		if (error)
		{
			outError = "cannot replace '" + FileSystem::PathToUtf8(keyPath) + "': " + error.message();
			return false;
		}
		if (!WritePng(path, entry.Image.Pixels, entry.Image.Width, entry.Image.Height, outError))
			return false;
		return FileSystem::WriteTextFile(keyPath, FormatKeyFile(key, entry.Details), outError);
	}

	size_t ThumbnailCache::PruneMissing() const
	{
		return Remove(false);
	}

	size_t ThumbnailCache::Clear() const
	{
		return Remove(true);
	}

	size_t ThumbnailCache::Remove(bool everything) const
	{
		std::error_code error;
		std::vector<std::filesystem::path> files;
		for (auto it = std::filesystem::directory_iterator(m_Directory, error); !error && it != std::filesystem::directory_iterator(); it.increment(error))
			files.push_back(it->path());
		if (error && error != std::errc::no_such_file_or_directory)
			BS_CORE_WARN("Thumbnails: cannot list '{}': {}", FileSystem::PathToUtf8(m_Directory), error.message());
		size_t removed = 0;
		const auto remove = [&](const std::filesystem::path& file) {
			std::error_code removeError;
			if (std::filesystem::remove(file, removeError))
				removed++;
			else if (removeError)
				BS_CORE_WARN("Thumbnails: cannot remove '{}': {}", FileSystem::PathToUtf8(file), removeError.message());
		};
		for (const std::filesystem::path& file : files)
		{
			if (file.extension() == ".key")
			{
				const auto stored = FileSystem::ReadTextFile(file);
				const auto keyFile = stored ? ParseKeyFile(*stored) : std::nullopt;
				const std::string source = keyFile ? SourceOfKey(keyFile->Key) : std::string();
				if (!everything && !source.empty() && FileSystem::Exists(m_ProjectDirectory / FileSystem::PathFromUtf8(source)))
					continue;
				remove(file);
				remove(std::filesystem::path(file).replace_extension(".png"));
			}
			else if (file.extension() == ".png" && (everything || !FileSystem::Exists(KeyPathOf(file))))
				remove(file);
		}
		return removed;
	}

}
