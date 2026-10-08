#pragma once

#include "Basalt/Core/UUID.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Basalt {

	class Entity;

	// Uniform, name-based access to every serializable component. Scene files, prefabs, editor undo
	// snapshots, Lua (Entity:GetComponent/SetComponent) and the automation API all go through these
	// functions, so a component behaves identically everywhere.
	//
	// Deserialize merges: only keys present in the JSON are changed. Unknown keys and wrong value types
	// are rejected with a descriptive error (and the component is left unchanged).
	struct ComponentInfo
	{
		// Name used in files and APIs, e.g. "Transform", "RigidBody".
		std::string Name;
		// Core components (Tag, Transform) exist on every entity and cannot be added or removed.
		bool IsCore = false;

		std::function<bool(Entity)> Has;
		// Adds a default-constructed component if missing.
		std::function<void(Entity)> Add;
		std::function<void(Entity)> Remove;
		std::function<nlohmann::json(Entity)> Serialize;
		// Returns false and fills outError on invalid input.
		std::function<bool(Entity, const nlohmann::json&, std::string& outError)> Deserialize;
		// Field names accepted by Deserialize, for error messages and tooling.
		std::vector<std::string> Fields;
		// Valid values of string-enum fields (e.g. RigidBody "Type"), for inspectors and validation messages.
		std::map<std::string, std::vector<std::string>> EnumOptions;
		// Rewrites the component's entity references (e.g. Joint "ConnectedEntity") through the map;
		// references not in the map are kept. Empty for components without references.
		std::function<void(Entity, const std::unordered_map<uint64_t, UUID>&)> RemapEntityReferences;
	};

	class ComponentRegistry
	{
	public:
		static const std::vector<ComponentInfo>& GetAll();
		static const ComponentInfo* Find(std::string_view name);
		// Comma-separated list of component names, for error messages.
		static std::string GetNameList();

		// Adds the component if needed and merges the JSON into it.
		static bool AddOrPatch(Entity entity, std::string_view name, const nlohmann::json& data, std::string& outError);

		// Entities copied with fresh UUIDs (prefab instances, duplicated trees) keep references to the
		// originals; this points references to entities in the copied set (old UUID -> new UUID) at the copies.
		static void RemapEntityReferences(Entity entity, const std::unordered_map<uint64_t, UUID>& remap);
	};

}
