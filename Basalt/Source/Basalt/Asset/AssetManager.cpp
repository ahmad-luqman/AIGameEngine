#include "Basalt/Asset/AssetManager.h"

#include "Basalt/Animation/AnimationGraph.h"
#include "Basalt/Asset/MeshImporter.h"
#include "Basalt/Core/Assert.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JobSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Core/MemoryTracker.h"
#include "Basalt/Core/Profiler.h"
#include "Basalt/Project/Project.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace Basalt {

	namespace {

		void CheckProject();

		template<typename T>
		struct LoadResult
		{
			Ref<T> Asset;
			std::string Error;
			// The other files a model reads (MeshSource::ExternalFiles), kept for a failed load too.
			std::vector<std::string> Dependencies;
		};

		template<typename T>
		struct CacheEntry : LoadResult<T>
		{
			JobHandle Job;
			// A deferred image starts only after this parent's completion fence.
			JobHandle ParentJob;
			Ref<const LoadResult<T>> Pending;
			// Prepared work stays on the main thread until its dependency is ready to schedule.
			std::function<void()> Work;
			// Other paths reported during import; dependencies are unknown until the load finishes.
			std::unordered_set<std::string> Changed;
		};

		std::unordered_map<std::string, CacheEntry<MeshSource>> s_Meshes;
		std::unordered_map<std::string, CacheEntry<TextureSource>> s_Textures;
		std::unordered_map<std::string, CacheEntry<const AnimationGraph>> s_Graphs;

		std::string Extension(const std::string& path)
		{
			return FileSystem::GetLowercaseExtension(FileSystem::PathFromUtf8(path));
		}

		Ref<MeshSource> LoadMesh(const std::string& key, const std::filesystem::path& path, const std::filesystem::path& root, std::vector<std::string>& outDependencies, std::string& outError)
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
			return MeshImporter::LoadGltf(path, key, root, outDependencies, outError);
		}

		Ref<TextureSource> LoadTexture(const std::string& key, const std::filesystem::path& path, const Ref<MeshSource>& mesh, const std::string& meshError, std::string& outError)
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
				if (!mesh)
				{
					outError = "cannot load '" + meshKey + "' for embedded image: " + (meshError.empty() ? "mesh unavailable" : meshError);
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
			return TextureSource::LoadFromFile(path, outError);
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
		CheckProject();
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

	namespace {
		template<typename T>
		void FailScheduling(CacheEntry<T>& entry, std::string error)
		{
			entry.Work = {};
			entry.Pending.reset();
			entry.ParentJob = {};
			entry.Job = {};
			entry.Error = std::move(error);
			BS_CORE_ERROR("AssetManager scheduling failed: {}", entry.Error);
		}

		template<typename T>
		void Start(const std::string& key, CacheEntry<T>& entry)
		{
			if (!entry.Work)
				return;
			// Scheduling never waits; blocking callers join their parent before looking up the entry.
			if (!entry.ParentJob.IsComplete())
				return;
			std::string schedulingError;
			try
			{
				entry.Job = JobSystem::Get().Submit(std::move(entry.Work));
				if (!entry.Job)
					schedulingError = "'" + key + "': asset load not scheduled";
			}
			catch (const std::exception& error)
			{
				FailScheduling(entry, "'" + key + "': scheduling exception: " + error.what());
				return;
			}
			catch (...)
			{
				FailScheduling(entry, "'" + key + "': unknown scheduling exception");
				return;
			}
			entry.Work = {};
			if (!schedulingError.empty())
				FailScheduling(entry, std::move(schedulingError));
		}

		template<typename T, typename F>
		void Prepare(const std::string& key, CacheEntry<T>& entry, const F& load)
		{
			try
			{
				auto result = CreateRef<LoadResult<T>>();
				entry.Pending = result;
				entry.Work = [key, result, load] {
					try
					{
						load(*result);
						if (result->Asset)
						{
							result->Asset->Key = key;
							result->Asset->UpdateMemoryUsage();
						}
					}
					catch (const std::exception& error)
					{
						result->Asset.reset();
						result->Error = "'" + key + "': worker exception: " + error.what();
					}
					catch (...)
					{
						result->Asset.reset();
						result->Error = "'" + key + "': unknown worker exception";
					}
					// Persistent hosts may publish later; one-shot hosts can exit without publishing.
					if (!result->Asset && result->Error.empty())
						result->Error = "'" + key + "': load produced no asset";
					if (!result->Asset && result.use_count() > 1)
						BS_CORE_ERROR("AssetManager '{}': {}", key, result->Error);
				};
				Start(key, entry);
			}
			catch (const std::exception& error)
			{
				FailScheduling(entry, "'" + key + "': preparing load exception: " + error.what());
			}
			catch (...)
			{
				FailScheduling(entry, "'" + key + "': unknown preparing load exception");
			}
		}

		template<typename T>
		void Publish(const std::string& key, CacheEntry<T>& entry)
		{
			Start(key, entry);
			if (!entry.Pending || entry.Work || !entry.Job.IsComplete())
				return;
			auto result = std::move(entry.Pending);
			// Dependency jobs may still read the completed result; publication must not move or write it.
			static_cast<LoadResult<T>&>(entry) = *result;
			if (entry.Job && !entry.Job.Succeeded())
			{
				entry.Asset.reset();
				if (entry.Error.empty())
					entry.Error = "'" + key + "': asset job failed (see job diagnostic)";
			}
			if (!entry.Asset && entry.Error.empty())
				entry.Error = "'" + key + "': load produced no asset";
			entry.Job = {};
			entry.ParentJob = {};
		}

		// Returns a changed dependency instead of exposing a result that needs another import.
		std::string PublishMesh(const std::string& key, CacheEntry<MeshSource>& entry)
		{
			if (entry.Pending)
			{
				if (!entry.Job.IsComplete())
					return {};
				for (const auto& file : entry.Pending->Dependencies)
					if (entry.Changed.contains(file))
						return file;
				entry.Changed.clear();
			}
			Publish(key, entry);
			return {};
		}

		void RetryMesh(const std::string& key, const std::string& dependency)
		{
			BS_CORE_INFO("AssetManager '{}': '{}' changed while loading; retrying", key, dependency);
			// Preserve requested children on automatic retry. Explicit Reload discards their entries.
			std::vector<std::string> images;
			for (const auto& [imageKey, entry] : s_Textures)
				if (AssetManager::GetSourceFile(imageKey) == key)
					images.push_back(imageKey);
			AssetManager::Reload(key);
			AssetManager::RequestMesh(key);
			for (const auto& image : images)
				AssetManager::RequestTexture(image);
		}

		// Project changes discard all project-dependent entries: workers own their paths and results.
		void CheckProject()
		{
			BS_CORE_ASSERT(!JobSystem::Get().IsInJob(), "Asset cache APIs belong to the main thread");
			static std::weak_ptr<Project> s_Project;
			static std::filesystem::path s_Root;
			const auto& project = Project::GetActive();
			const auto root = Project::ResolvePath("");
			if (s_Project.lock() != project || s_Root != root)
			{
				// Completed built-ins have no project dependency and stay useful across project switches.
				std::erase_if(s_Meshes, [](const auto& entry) { return entry.second.Pending || !Primitives::IsBuiltinKey(entry.first); });
				s_Textures.clear();
				s_Graphs.clear();
				s_Project = project;
				s_Root = root;
			}
		}
	}

	Ref<MeshSource> AssetManager::RequestMesh(const std::string& key)
	{
		CheckProject();
		if (key.empty())
			return nullptr;
		auto [it, inserted] = s_Meshes.try_emplace(key);
		auto& entry = it->second;
		if (inserted)
		{
			const auto path = Project::ResolvePath(key);
			const auto root = Project::GetActive() ? Project::GetActive()->GetDirectory() : path.parent_path();
			Prepare(key, entry, [key, path, root](LoadResult<MeshSource>& result) {
				result.Asset = LoadMesh(key, path, root, result.Dependencies, result.Error);
			});
		}
		// Request calls never publish merely because a worker has finished.
		return entry.Asset;
	}

	Ref<MeshSource> AssetManager::GetMesh(const std::string& key)
	{
		RequestMesh(key);
		for (;;)
		{
			auto it = s_Meshes.find(key);
			if (it == s_Meshes.end())
				return nullptr;
			if (!it->second.Pending)
				return it->second.Asset;
			// Wait may execute jobs. Retain only the fence across it, then validate a fresh lookup.
			const auto job = it->second.Job;
			JobSystem::Get().Wait(job);
			it = s_Meshes.find(key);
			if (it == s_Meshes.end())
				return nullptr;
			const auto dependency = PublishMesh(key, it->second);
			if (!dependency.empty())
				RetryMesh(key, dependency);
		}
	}

	Ref<MeshSource> AssetManager::FindLoadedMesh(const std::string& key)
	{
		CheckProject();
		const auto it = s_Meshes.find(key);
		return it != s_Meshes.end() ? it->second.Asset : nullptr;
	}

	Ref<TextureSource> AssetManager::RequestTexture(const std::string& key)
	{
		CheckProject();
		if (key.empty())
			return nullptr;
		if (auto existing = s_Textures.find(key); existing != s_Textures.end())
			return existing->second.Asset;
		Ref<MeshSource> mesh;
		Ref<const LoadResult<MeshSource>> parentResult;
		JobHandle parentJob;
		std::string parentError;
		const size_t marker = key.find("#image/");
		if (marker != std::string::npos)
		{
			const auto meshKey = key.substr(0, marker);
			mesh = RequestMesh(meshKey);
			if (auto parent = s_Meshes.find(meshKey); parent != s_Meshes.end())
			{
				parentResult = parent->second.Pending;
				parentJob = parent->second.Job;
				parentError = parent->second.Error;
			}
		}
		auto [it, inserted] = s_Textures.try_emplace(key);
		auto& entry = it->second;
		if (inserted)
		{
			const auto path = Project::ResolvePath(key);
			entry.ParentJob = std::move(parentJob);
			Prepare(key, entry, [key, path, mesh, parentResult, parentError](LoadResult<TextureSource>& result) {
				BS_PROFILE_SCOPE("Decode texture");
				const auto parent = parentResult ? parentResult->Asset : mesh;
				const auto& error = parentResult ? parentResult->Error : parentError;
				result.Asset = LoadTexture(key, path, parent, error, result.Error);
			});
		}
		return entry.Asset;
	}

	Ref<TextureSource> AssetManager::GetTexture(const std::string& key)
	{
		// Joining the parent can retry it and recreate this image entry; do so before looking it up.
		if (key.find("#image/") != std::string::npos)
			GetMesh(GetSourceFile(key));
		RequestTexture(key);
		auto it = s_Textures.find(key);
		if (it == s_Textures.end())
			return nullptr;
		if (it->second.Pending)
		{
			Start(key, it->second);
			const auto job = it->second.Job;
			JobSystem::Get().Wait(job);
			it = s_Textures.find(key);
			if (it == s_Textures.end())
				return nullptr;
			Publish(key, it->second);
		}
		return it->second.Asset;
	}

	bool AssetManager::IsLoading(const std::string& key)
	{
		CheckProject();
		if (auto it = s_Meshes.find(key); it != s_Meshes.end() && it->second.Pending)
			return true;
		if (auto it = s_Textures.find(key); it != s_Textures.end() && it->second.Pending)
			return true;
		return false;
	}

	void AssetManager::PublishCompleted()
	{
		BS_PROFILE_SCOPE("Publish assets");
		CheckProject();
		std::vector<std::pair<std::string, std::string>> retry;
		for (auto& [key, entry] : s_Meshes)
		{
			const auto dependency = PublishMesh(key, entry);
			if (!dependency.empty())
				retry.emplace_back(key, dependency);
		}
		// Retry after traversal, since replacing entries can invalidate map iterators.
		for (const auto& [key, dependency] : retry)
			RetryMesh(key, dependency);
		for (auto& [key, entry] : s_Textures)
		{
			// Loading, retried and late-completing parents must be validated before exposing their images.
			if (const auto parent = s_Meshes.find(GetSourceFile(key)); parent != s_Meshes.end() && parent->second.Pending)
				continue;
			Publish(key, entry);
		}
	}

	void AssetManager::WaitForLoads()
	{
		// Snapshot keys rather than retaining map entries across cooperative waits.
		const auto keys = GetLoadedKeys();
		for (const auto& key : keys)
			if (s_Meshes.contains(key))
				GetMesh(key);
		for (const auto& key : keys)
			if (s_Textures.contains(key))
				GetTexture(key);
		PublishCompleted();
	}

	std::string AssetManager::GetError(const std::string& key)
	{
		CheckProject();
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
		CheckProject();
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
		CheckProject();
		if (paths.empty())
			return {};
		const std::unordered_set<std::string> changed(paths.begin(), paths.end());
		std::set<std::string> keys;
		for (auto& [key, entry] : s_Meshes)
		{
			if (Primitives::IsBuiltinKey(key))
				continue;
			if (entry.Pending)
				entry.Changed.insert(paths.begin(), paths.end());
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
		CheckProject();
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
	{
		CheckProject();
		m_Meshes = SortedKeys(s_Meshes);
		m_Textures = SortedKeys(s_Textures);
		m_Graphs = SortedKeys(s_Graphs);
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
