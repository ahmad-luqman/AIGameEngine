#pragma once

#include "Basalt/Asset/MeshSource.h"
#include "Basalt/Asset/TextureSource.h"
#include "Basalt/Core/Base.h"
#include "Basalt/Scene/Entity.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	class Scene;

	// glTF 2.0 (.gltf / .glb) import: geometry, metallic-roughness materials, textures and node hierarchy.
	class MeshImporter
	{
	public:
		// key: the project-relative asset key used to name embedded textures ("<key>#image/<n>").
		static Ref<MeshSource> LoadGltf(const std::filesystem::path& path, const std::string& key, std::string& outError);
		// Also lists the files the glTF names (MeshSource::ExternalFiles), even when loading fails once it has parsed
		// (a missing buffer), so a fix to one of them can retry the load.
		static Ref<MeshSource> LoadGltf(const std::filesystem::path& path, const std::string& key, std::vector<std::string>& outExternalFiles, std::string& outError);
		// Worker-safe overload: projectRoot is captured by the requesting thread.
		static Ref<MeshSource> LoadGltf(const std::filesystem::path& path, const std::string& key, const std::filesystem::path& projectRoot, std::vector<std::string>& outExternalFiles, std::string& outError);
		// The asset key of a file a glTF names by URI (an external buffer or image): the decoded URI resolved against
		// the folder of the model's key, '/'-separated UTF-8 ("Assets/Models/Ship.bin"), as FileWatcher reports it.
		// nullopt for no URI or a data: URI (embedded).
		static std::optional<std::string> GetExternalFileKey(const std::string& modelKey, const char* uri);

		// What a glTF's sub-asset keys can name, read from its JSON (buffers are not loaded): "#material/<n>" below
		// Materials, "#image/<n>" an image stored in the file (an empty ImageUris entry: a buffer view or data: URI).
		struct Contents
		{
			// Per image: empty when embedded, else the external file's URI (which loads as its own texture).
			std::vector<std::string> ImageUris;
			size_t Materials = 0;
		};
		// nullopt with the parser's reason when the file is not a readable glTF.
		static std::optional<Contents> Inspect(const std::filesystem::path& path, std::string& outError);
		// The problem with a "<model>#image/<n>" or "<model>#material/<n>" key given its model's contents, or nullopt.
		static std::optional<std::string> CheckSubAsset(const Contents& contents, std::string_view key);

		// Static models become an editable entity hierarchy; skinned/animated models use one Animator root.
		// Returns the root entity named after the file.
		static Entity InstantiateHierarchy(Scene& scene, const MeshSource& source, const std::string& name, Entity parent);
	};

}
