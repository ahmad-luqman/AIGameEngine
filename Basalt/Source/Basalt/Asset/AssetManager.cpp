#include "Basalt/Asset/AssetManager.h"

#include "Basalt/Asset/MeshImporter.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Project/Project.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <unordered_map>

namespace Basalt {

	namespace {

		template<typename T>
		struct CacheEntry
		{
			Ref<T> Asset;
			std::string Error;
		};

		std::unordered_map<std::string, CacheEntry<MeshSource>> s_Meshes;
		std::unordered_map<std::string, CacheEntry<TextureSource>> s_Textures;

		std::string Extension(const std::string& path)
		{
			std::string extension = std::filesystem::path(path).extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return extension;
		}

		Ref<MeshSource> LoadMesh(const std::string& key, std::string& outError)
		{
			if (Primitives::IsBuiltinKey(key))
			{
				Ref<MeshSource> mesh = Primitives::CreateFromKey(key);
				if (!mesh)
					outError = "unknown built-in mesh '" + key + "' (valid: builtin://Cube, Sphere, Plane, Cylinder, Capsule)";
				return mesh;
			}
			if (!AssetManager::IsMeshFile(key))
			{
				outError = "'" + key + "' is not a supported mesh file (.gltf, .glb)";
				return nullptr;
			}
			return MeshImporter::LoadGltf(Project::ResolvePath(key), key, outError);
		}

		Ref<TextureSource> LoadTexture(const std::string& key, std::string& outError)
		{
			const size_t marker = key.find("#image/");
			if (marker != std::string::npos)
			{
				const std::string meshKey = key.substr(0, marker);
				const std::string indexText = key.substr(marker + 7);
				uint32_t index = 0;
				const auto [end, parseError] = std::from_chars(indexText.data(), indexText.data() + indexText.size(), index);
				if (indexText.empty() || parseError != std::errc() || end != indexText.data() + indexText.size())
				{
					outError = "invalid embedded image key '" + key + "'";
					return nullptr;
				}
				Ref<MeshSource> mesh = AssetManager::GetMesh(meshKey);
				if (!mesh)
				{
					outError = "cannot load '" + meshKey + "' for embedded image: " + AssetManager::GetError(meshKey);
					return nullptr;
				}
				if (index >= mesh->EmbeddedImages.size() || mesh->EmbeddedImages[index].empty())
				{
					outError = "'" + meshKey + "' has no embedded image " + indexText;
					return nullptr;
				}
				Ref<TextureSource> texture = TextureSource::LoadFromMemory(mesh->EmbeddedImages[index].data(), mesh->EmbeddedImages[index].size(), outError);
				if (!texture)
					outError = key + ": " + outError;
				return texture;
			}
			return TextureSource::LoadFromFile(Project::ResolvePath(key), outError);
		}

	}

	Ref<MeshSource> AssetManager::GetMesh(const std::string& key)
	{
		if (key.empty())
			return nullptr;
		auto it = s_Meshes.find(key);
		if (it != s_Meshes.end())
			return it->second.Asset;

		CacheEntry<MeshSource> entry;
		entry.Asset = LoadMesh(key, entry.Error);
		if (entry.Asset)
			entry.Asset->Key = key;
		else
			BS_CORE_ERROR("AssetManager: {}", entry.Error);
		return s_Meshes.emplace(key, std::move(entry)).first->second.Asset;
	}

	Ref<TextureSource> AssetManager::GetTexture(const std::string& key)
	{
		if (key.empty())
			return nullptr;
		auto it = s_Textures.find(key);
		if (it != s_Textures.end())
			return it->second.Asset;

		CacheEntry<TextureSource> entry;
		entry.Asset = LoadTexture(key, entry.Error);
		if (entry.Asset)
			entry.Asset->Key = key;
		else
			BS_CORE_ERROR("AssetManager: {}", entry.Error);
		return s_Textures.emplace(key, std::move(entry)).first->second.Asset;
	}

	std::string AssetManager::GetError(const std::string& key)
	{
		if (auto it = s_Meshes.find(key); it != s_Meshes.end())
			return it->second.Error;
		if (auto it = s_Textures.find(key); it != s_Textures.end())
			return it->second.Error;
		return {};
	}

	void AssetManager::Reload(const std::string& key)
	{
		s_Meshes.erase(key);
		s_Textures.erase(key);
		// Embedded images belong to their mesh file.
		const std::string prefix = key + "#image/";
		for (auto it = s_Textures.begin(); it != s_Textures.end();)
		{
			if (it->first.rfind(prefix, 0) == 0)
				it = s_Textures.erase(it);
			else
				++it;
		}
	}

	void AssetManager::Clear()
	{
		s_Meshes.clear();
		s_Textures.clear();
	}

	Entity AssetManager::ImportModel(Scene& scene, const std::string& key, Entity parent, std::string& outError)
	{
		Ref<MeshSource> mesh = GetMesh(key);
		if (!mesh)
		{
			outError = GetError(key);
			return {};
		}
		const std::string name = Primitives::IsBuiltinKey(key) ? key.substr(10) : std::filesystem::path(key).stem().string();
		return MeshImporter::InstantiateHierarchy(scene, *mesh, name, parent);
	}

	std::vector<std::string> AssetManager::ListAssets(const std::vector<std::string>& extensions)
	{
		std::vector<std::string> result;
		const std::filesystem::path root = Project::ResolvePath(Project::AssetDirectoryName);
		std::error_code error;
		if (!std::filesystem::is_directory(root, error))
			return result;

		for (auto it = std::filesystem::recursive_directory_iterator(root, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
		{
			if (!it->is_regular_file(error))
				continue;
			const std::string path = Project::MakeRelative(it->path());
			if (extensions.empty() || std::find(extensions.begin(), extensions.end(), Extension(path)) != extensions.end())
				result.push_back(path);
		}
		std::sort(result.begin(), result.end());
		return result;
	}

	bool AssetManager::IsMeshFile(const std::string& path)
	{
		const std::string extension = Extension(path);
		return extension == ".gltf" || extension == ".glb";
	}

	bool AssetManager::IsTextureFile(const std::string& path)
	{
		const std::string extension = Extension(path);
		return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga" || extension == ".bmp" || extension == ".hdr";
	}

}
