#pragma once

#include "Basalt/Asset/MeshSource.h"
#include "Basalt/Asset/TextureSource.h"
#include "Basalt/Core/Base.h"
#include "Basalt/Scene/Entity.h"

#include <string>
#include <vector>

namespace Basalt {

	class Scene;

	// Loads and caches CPU-side assets by key. Keys are project-relative paths ("Assets/Models/Ship.glb"),
	// built-in meshes ("builtin://Cube"), or embedded glTF images ("Assets/Models/Ship.glb#image/0").
	//
	// Failed loads are cached too, so a missing file is reported once rather than every frame; call
	// Reload() (or Clear()) after fixing the file. Main thread only.
	class AssetManager
	{
	public:
		static Ref<MeshSource> GetMesh(const std::string& key);
		static Ref<TextureSource> GetTexture(const std::string& key);
		// Error message from the last failed load of key (empty if it loaded or was never requested).
		static std::string GetError(const std::string& key);

		// Drops a cached asset so the next Get reloads it from disk.
		static void Reload(const std::string& key);
		static void Clear();

		// Loads a model and instantiates its node hierarchy into the scene. Returns the root entity.
		static Entity ImportModel(Scene& scene, const std::string& key, Entity parent, std::string& outError);

		// Project-relative paths of every file under the asset directory with one of the extensions
		// (lower-case, including the dot). Empty extension list returns every file.
		static std::vector<std::string> ListAssets(const std::vector<std::string>& extensions = {});
		static bool IsMeshFile(const std::string& path);
		static bool IsTextureFile(const std::string& path);
	};

}
