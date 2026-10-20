#pragma once

#include "Basalt/Asset/MeshSource.h"
#include "Basalt/Asset/TextureSource.h"
#include "Basalt/Core/Base.h"
#include "Basalt/Scene/Entity.h"

#include <string>
#include <vector>

namespace Basalt {

	class Scene;
	struct AnimationGraph;

	// Loads and caches CPU-side assets by key. Keys are project-relative paths ("Assets/Models/Ship.glb"),
	// built-in meshes ("builtin://Cube"), embedded glTF images ("Assets/Models/Ship.glb#image/0") or imported glTF
	// materials ("Assets/Models/Ship.glb#material/0").
	//
	// Failed loads are cached too, so a missing file is reported once rather than every frame; call
	// Reload() (or Clear()) after fixing the file. Cache APIs are main thread only; workers own isolated results.
	class AssetManager
	{
	public:
		static Ref<const AnimationGraph> GetAnimationGraph(const std::string& key);
		// A material file (.bmaterial, parsed on the main thread) or a model's imported material ("<model>#material/<n>",
		// n below the model's glTF material count; it shares the model's lifetime). GetMaterial loads the model if needed;
		// RequestMaterial returns null while the model streams in and caches nothing until it is published. Failures are
		// cached and logged once. Texture keys inside are loaded separately (GetTexture/RequestTexture).
		static Ref<const MaterialData> GetMaterial(const std::string& key);
		static Ref<const MaterialData> RequestMaterial(const std::string& key);
		// Starts a deduplicated worker load and returns only already-published assets. Call PublishCompleted each frame before polling.
		// GetMesh/GetTexture join and publish only the requested key and its mesh dependency, including retries.
		static Ref<MeshSource> RequestMesh(const std::string& key);
		static Ref<TextureSource> RequestTexture(const std::string& key);
		// True until publication, including when a worker has finished. Does not start a load.
		static bool IsLoading(const std::string& key);
		// Main-thread fence; scene rendering and UI frame building call this even without a camera.
		// Headless callers must publish explicitly (asset.preload does so when polled).
		static void PublishCompleted();
		// Joins requested loads and dependencies, retries changed dependencies, then publishes.
		static void WaitForLoads();
		static Ref<MeshSource> GetMesh(const std::string& key);
		// The mesh if it is already loaded, without loading it (tooltips that must not stall a frame).
		static Ref<MeshSource> FindLoadedMesh(const std::string& key);
		static Ref<TextureSource> GetTexture(const std::string& key);
		// Error message from the last failed load of key (empty while pending, if loaded, or if never requested).
		static std::string GetError(const std::string& key);

		// Drops a cached asset so the next Get or Request reloads it from disk.
		static void Reload(const std::string& key);
		// Drops what a change to these project-relative files affects (including animation graphs and materials): the files' own cache entries
		// (with a model's embedded images and imported materials), models that read one of them (external buffers and images; also for a model that failed
		// to load once its glTF parsed, so a buffer that appears retries it), and material files whose textures (or their
		// sidecars) changed or, for a reference that did not resolve, whenever metadata changed. Failed loads count, so a fixed file is tried
		// again. Returns the keys dropped immediately, sorted; unrequested files drop nothing. A change to a pending mesh
		// file drops it immediately. Changes to other files are retained until its dependencies are known,
		// then retry at publication only if affected (with a diagnostic naming the changed dependency).
		// Built-in meshes are independent of file changes.
		static std::vector<std::string> ReloadChanged(const std::vector<std::string>& paths);
		// The file an asset key loads from: the model of an embedded image or imported material ("Ship.glb#image/0" ->
		// "Ship.glb"), the key itself otherwise.
		static std::string GetSourceFile(const std::string& key);
		static void Clear();
		// Keys of every cache entry, failed loads included, sorted.
		static std::vector<std::string> GetLoadedKeys();

		// Drops, when it ends, every cache entry that did not exist when it began: an editor preview loads a model to
		// render it once, and must not keep it (or its textures) in memory. Entries present at the start stay; one that
		// was dropped and made again meanwhile counts as present. An entry the open scene first requests during the scope
		// is dropped too and simply loads again on its next use.
		class TemporaryLoads
		{
		public:
			[[nodiscard]] TemporaryLoads();
			~TemporaryLoads();
			TemporaryLoads(const TemporaryLoads&) = delete;
			TemporaryLoads& operator=(const TemporaryLoads&) = delete;

		private:
			std::vector<std::string> m_Meshes;
			std::vector<std::string> m_Textures;
			std::vector<std::string> m_Graphs;
			std::vector<std::string> m_Materials;
		};

		// Imports a static entity hierarchy or a single Animator root for a skinned/animated model.
		static Entity ImportModel(Scene& scene, const std::string& key, Entity parent, std::string& outError);

		// Project-relative paths of every file under the asset directory with one of the extensions
		// (lower-case, including the dot). Metadata is excluded. On an incomplete scan, returns the files found
		// so far and sets *outError when provided; successful scans clear it.
		static std::vector<std::string> ListAssets(const std::vector<std::string>& extensions = {}, std::string* outError = nullptr);
		static bool IsMeshFile(const std::string& path);
		static bool IsTextureFile(const std::string& path);
	};

}
