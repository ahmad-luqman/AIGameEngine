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

		// Recreates the mesh's node hierarchy as entities (one per node, MeshComponents on mesh nodes).
		// Returns the root entity named after the file.
		static Entity InstantiateHierarchy(Scene& scene, const MeshSource& source, const std::string& name, Entity parent);
	};

}
