#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Scene/Entity.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace Basalt {

	class Scene;

	// JSON (de)serialization of scenes (.bscene) and prefabs (.bprefab).
	//
	// Scene file layout:
	//   { "Format": "BasaltScene", "Version": 1, "Name": "...",
	//     "Renderer": { ...RendererSettings... }, "Physics": { "Gravity": [x,y,z], ... },
	//     "Entities": [ { "ID": 123, "Name": "Player", "Parent": 0, "Components": { "Transform": {...}, ... } } ] }
	// Entities are written parents-first in hierarchy order; "Parent" is 0 for roots.
	// A prefab has the same "Entities" array; the first entity is the prefab root.
	class SceneSerializer
	{
	public:
		static constexpr int FormatVersion = 1;

		static nlohmann::json SerializeScene(Scene& scene);
		// Clears nothing: entities are added to the (normally empty) scene. Returns false on invalid data,
		// leaving any entities created before the error in place, with outError describing the problem.
		static bool DeserializeScene(Scene& scene, const nlohmann::json& data, std::string& outError);

		static bool SaveScene(Scene& scene, const std::filesystem::path& path, std::string& outError);
		static Ref<Scene> LoadScene(const std::filesystem::path& path, std::string& outError);

		// Entity plus all descendants, as an "Entities" array (root first).
		static nlohmann::json SerializeEntityTree(Entity root);
		// Instantiates an entity tree with fresh UUIDs under parent (or as a root). Returns the new root.
		static Entity DeserializeEntityTree(Scene& scene, const nlohmann::json& entities, Entity parent, std::string& outError);

		static bool SavePrefab(Entity root, const std::filesystem::path& path, std::string& outError);
		// Instantiates a prefab file into the scene. The new root gets a PrefabComponent naming prefabPath.
		static Entity InstantiatePrefab(Scene& scene, const std::filesystem::path& absolutePath, const std::string& prefabPath, Entity parent, std::string& outError);

		static nlohmann::json SerializeEntity(Entity entity);

		// 64-bit hash (16 hex digits) of the serialized scene state, for determinism and replay checks.
		// Entity UUIDs (random for runtime spawns) are replaced by hierarchy-order indices wherever they
		// appear, so two runs that reach the same state hash equally. Rotations are hashed as quaternions
		// rather than the file's Euler angles, so equal physics states hash equally on every platform.
		static std::string ComputeStateHash(Scene& scene);
		// Applies an entity object's "Name" and "Components" to an existing entity.
		static bool DeserializeEntityComponents(Entity entity, const nlohmann::json& data, std::string& outError);
	};

}
