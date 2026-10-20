#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Scene/Entity.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	class Scene;

	// JSON (de)serialization of scenes (.bscene) and prefabs (.bprefab).
	//
	// Scene file layout:
	//   { "Format": "BasaltScene", "Version": 3, "Name": "...",
	//     "Renderer": { ...RendererSettings... }, "Physics": { "Gravity": [x,y,z], ... },
	//     "Entities": [ { "ID": 123, "Name": "Player", "Parent": 0, "Components": { "Transform": {...}, ... } } ] }
	// Entities are written parents-first in hierarchy order; "Parent" is 0 for roots.
	// A prefab has the same "Entities" array; the first entity is the prefab root.
	//
	// Versions: 3 writes declared asset fields as {UUID, Path} objects on disk; legacy strings remain readable.
	// Version 2 writes Physics.FixedTimestep only as a scene override of the project's step. Version 1 always
	// wrote it, so a version-1 file holding the old default (1/60 s) is read as "no override".
	class SceneSerializer
	{
	public:
		static constexpr int FormatVersion = 3;
		// Encodes declared asset fields for disk persistence, creating sidecars when needed. Missing and
		// non-project references stay strings. onlyPath limits migration to that asset and its embedded images.
		// Uses the project's current index; batch writers refresh before calling. No-op without a project.
		static bool EncodeAssetReferences(nlohmann::json& data, std::string& outError, const std::string& onlyPath = {});

		static nlohmann::json SerializeScene(Scene& scene);
		// Clears nothing: entities are added to the (normally empty) scene. Returns false on invalid data,
		// leaving any entities created before the error in place, with outError describing the problem.
		static bool DeserializeScene(Scene& scene, const nlohmann::json& data, std::string& outError);

		// Both record the file as the scene's path (Scene::GetPath).
		static bool SaveScene(Scene& scene, const std::filesystem::path& path, std::string& outError);
		static Ref<Scene> LoadScene(const std::filesystem::path& path, std::string& outError);
		// Reads and parses a scene file without applying it; errors name the file as displayName (a missing file's
		// also gives the resolved path).
		static std::optional<nlohmann::json> ReadSceneFile(const std::filesystem::path& path, const std::string& displayName, std::string& outError);
		// The path Scene::GetPath reports for a scene file: project-relative inside the active project,
		// otherwise absolute (relative input is made absolute first); normalized, with '/' separators.
		static std::string ScenePathOf(const std::filesystem::path& path);

		// Entity plus all descendants, as an "Entities" array (root first).
		static nlohmann::json SerializeEntityTree(Entity root);
		// Instantiates an entity tree with fresh UUIDs under parent (or as a root). Returns the new root.
		static Entity DeserializeEntityTree(Scene& scene, const nlohmann::json& entities, Entity parent, std::string& outError);

		static bool SavePrefab(Entity root, const std::filesystem::path& path, std::string& outError);
		// Instantiates a prefab file into the scene. The new root gets a PrefabComponent naming prefabPath.
		static Entity InstantiatePrefab(Scene& scene, const std::filesystem::path& absolutePath, const std::string& prefabPath, Entity parent, std::string& outError);

		static nlohmann::json SerializeEntity(Entity entity);

		// 64-bit hash (16 hex digits) of the serialized scene state, for determinism and replay checks. It covers
		// state, not file format (Format and Version are left out), and the physics step the simulation uses (the
		// scene's override or the project's), so changing the scene's override or the project's step moves it.
		// Entity UUIDs (seeded during Play/Simulate) are replaced by hierarchy-order indices wherever they
		// appear, so two runs that reach the same state hash equally. Transform rotations are hashed as
		// quaternions rather than the file's Euler angles, so equal physics states hash equally on every platform.
		static std::string ComputeStateHash(Scene& scene);
		// Applies an entity object's "Name" and "Components" to an existing entity.
		static bool DeserializeEntityComponents(Entity entity, const nlohmann::json& data, std::string& outError);

	private:
		static bool CreateEntities(Scene& scene, const nlohmann::json& entities, bool remapIDs, Entity rootParent, std::vector<Entity>& outCreated, std::string& outError);
	};

}
