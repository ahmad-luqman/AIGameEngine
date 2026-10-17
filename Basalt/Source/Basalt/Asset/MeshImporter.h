#pragma once

#include "Basalt/Asset/MeshSource.h"
#include "Basalt/Asset/TextureSource.h"
#include "Basalt/Core/Base.h"
#include "Basalt/Scene/Entity.h"

#include <filesystem>
#include <string>

namespace Basalt {

	class Scene;

	// glTF 2.0 (.gltf / .glb) import: geometry, metallic-roughness materials, textures and node hierarchy.
	class MeshImporter
	{
	public:
		// key: the project-relative asset key used to name embedded textures ("<key>#image/<n>").
		static Ref<MeshSource> LoadGltf(const std::filesystem::path& path, const std::string& key, std::string& outError);
		// The asset key of a file a glTF names by URI (an external buffer or image): the decoded URI resolved against
		// the folder of the model's key, '/'-separated UTF-8 ("Assets/Models/Ship.bin"), as FileWatcher reports it.
		static std::string GetExternalFileKey(const std::string& modelKey, const char* uri);

		// Static models become an editable entity hierarchy; skinned/animated models use one Animator root.
		// Returns the root entity named after the file.
		static Entity InstantiateHierarchy(Scene& scene, const MeshSource& source, const std::string& name, Entity parent);
	};

}
