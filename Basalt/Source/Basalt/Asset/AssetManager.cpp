#include "Basalt/Asset/AssetManager.h"

#include "Basalt/Animation/AnimationGraph.h"
#include "Basalt/Asset/MeshImporter.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Project/Project.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace Basalt {

	namespace {

		template<typename T>
		struct CacheEntry
		{
			Ref<T> Asset;
			std::string Error;
			// The other files a model reads (MeshSource::ExternalFiles), kept for a failed load too.
			std::vector<std::string> Dependencies;
		};

		std::unordered_map<std::string, CacheEntry<MeshSource>> s_Meshes;
		std::unordered_map<std::string, CacheEntry<TextureSource>> s_Textures;
		std::unordered_map<std::string, CacheEntry<const AnimationGraph>> s_Graphs;

		std::string Extension(const std::string& path)
		{
			return FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(path));
		}

		Ref<MeshSource> LoadMesh(const std::string& key, std::vector<std::string>& outDependencies, std::string& outError)
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
			return MeshImporter::LoadGltf(Project::ResolvePath(key), key, outDependencies, outError);
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

		Ref<const AnimationGraph> LoadAnimationGraph(const std::string& key, std::string& outError)
		{
			const auto text = FileSystem::ReadTextFile(Project::ResolvePath(key));
			if (!text)
				outError = "cannot read animation graph '" + key + "'";
			else
			{
				const auto data = ParseJson(*text, outError);
				if (!data.is_discarded())
					if (auto graph = AnimationGraph::FromJson(data, outError))
						return CreateRef<const AnimationGraph>(std::move(*graph));
			}
			return nullptr;
		}

	}

	Ref<const AnimationGraph> AssetManager::GetAnimationGraph(const std::string& key)
	{
		if (key.empty())
			return nullptr;
		if (auto it = s_Graphs.find(key); it != s_Graphs.end())
			return it->second.Asset;
		CacheEntry<const AnimationGraph> entry;
		entry.Asset = LoadAnimationGraph(key, entry.Error);
		if (!entry.Asset)
			BS_CORE_ERROR("Animation graph '{}': {}", key, entry.Error);
		return s_Graphs.emplace(key, std::move(entry)).first->second.Asset;
	}

	Ref<MeshSource> AssetManager::GetMesh(const std::string& key)
	{
		if (key.empty())
			return nullptr;
		auto it = s_Meshes.find(key);
		if (it != s_Meshes.end())
			return it->second.Asset;

		CacheEntry<MeshSource> entry;
		entry.Asset = LoadMesh(key, entry.Dependencies, entry.Error);
		if (entry.Asset)
			entry.Asset->Key = key;
		else
			BS_CORE_ERROR("AssetManager: {}", entry.Error);
		return s_Meshes.emplace(key, std::move(entry)).first->second.Asset;
	}

	Ref<MeshSource> AssetManager::FindLoadedMesh(const std::string& key)
	{
		const auto it = s_Meshes.find(key);
		return it != s_Meshes.end() ? it->second.Asset : nullptr;
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
		if (auto it = s_Graphs.find(key); it != s_Graphs.end())
			return it->second.Error;
		return {};
	}

	void AssetManager::Reload(const std::string& key)
	{
		s_Graphs.erase(key);
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

	std::vector<std::string> AssetManager::ReloadChanged(const std::vector<std::string>& paths)
	{
		if (paths.empty())
			return {};
		const std::unordered_set<std::string> changed(paths.begin(), paths.end());
		std::set<std::string> keys;
		for (const auto& [key, entry] : s_Meshes)
		{
			const bool readsChangedFile = std::any_of(entry.Dependencies.begin(), entry.Dependencies.end(), [&](const std::string& file) { return changed.contains(file); });
			if (changed.contains(key) || readsChangedFile)
				keys.insert(key);
		}
		for (const auto& [key, entry] : s_Textures)
		{
			// An embedded image goes with its model file (Reload drops those).
			const std::string source = GetSourceFile(key);
			if (changed.contains(source) || keys.contains(source))
				keys.insert(key);
		}
		for (const auto& [key, entry] : s_Graphs)
			if (changed.contains(key))
				keys.insert(key);
		for (const std::string& key : keys)
			Reload(key);
		return { keys.begin(), keys.end() };
	}

	std::string AssetManager::GetSourceFile(const std::string& key)
	{
		const size_t marker = key.find("#image/");
		return marker != std::string::npos ? key.substr(0, marker) : key;
	}

	void AssetManager::Clear()
	{
		s_Graphs.clear();
		s_Meshes.clear();
		s_Textures.clear();
	}

	std::vector<std::string> AssetManager::GetLoadedKeys()
	{
		std::vector<std::string> keys;
		keys.reserve(s_Meshes.size() + s_Textures.size() + s_Graphs.size());
		for (const auto& [key, entry] : s_Meshes)
			keys.push_back(key);
		for (const auto& [key, entry] : s_Textures)
			keys.push_back(key);
		for (const auto& [key, entry] : s_Graphs)
			keys.push_back(key);
		std::sort(keys.begin(), keys.end());
		keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
		return keys;
	}

	namespace {

		template<typename T>
		std::vector<std::string> SortedKeys(const std::unordered_map<std::string, CacheEntry<T>>& cache)
		{
			std::vector<std::string> keys;
			keys.reserve(cache.size());
			for (const auto& [key, entry] : cache)
				keys.push_back(key);
			std::sort(keys.begin(), keys.end());
			return keys;
		}

		// Erases the entries whose key is not in `before` (sorted). Directly rather than through Reload, which would also
		// drop the embedded images of a model that was already loaded.
		template<typename T>
		void EraseAddedSince(std::unordered_map<std::string, CacheEntry<T>>& cache, const std::vector<std::string>& before)
		{
			std::erase_if(cache, [&](const auto& entry) { return !std::binary_search(before.begin(), before.end(), entry.first); });
		}

	}

	AssetManager::TemporaryLoads::TemporaryLoads()
		: m_Meshes(SortedKeys(s_Meshes))
		, m_Textures(SortedKeys(s_Textures))
		, m_Graphs(SortedKeys(s_Graphs))
	{
	}

	AssetManager::TemporaryLoads::~TemporaryLoads()
	{
		EraseAddedSince(s_Meshes, m_Meshes);
		EraseAddedSince(s_Textures, m_Textures);
		EraseAddedSince(s_Graphs, m_Graphs);
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
