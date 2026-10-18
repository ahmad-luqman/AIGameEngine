#pragma once

#include "Basalt/Core/UUID.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Basalt {

	class Entity;

	enum class ComponentFieldKind : uint8_t
	{
		Number = 0,
		Integer,
		Boolean,
		String,
		Object,
		// An entity UUID (0 = none).
		Entity,
		// A variable-length array of objects described by ComponentFieldType::Element.
		List,
		// A string of ComponentInfo::EnumOptions.
		Enum // keep last: ComponentFieldType::ToString has one name per kind
	};

	struct ComponentObjectSchema;

	// The JSON type of a component field: one value of a kind (Count 0), an array of Count values of that kind
	// (vectors, colors, Euler degrees, boolean vectors, per-axis enums), or a List of up to MaxCount objects
	// whose fields Element describes.
	struct ComponentFieldType
	{
		ComponentFieldKind Kind = ComponentFieldKind::Number;
		uint8_t Count = 0;
		// List only: the largest accepted length and the shape of every element.
		uint32_t MaxCount = 0;
		std::shared_ptr<const ComponentObjectSchema> Element;

		// "number", "integer", "boolean", "string", "object", "entity" or "enum", with "[Count]" for an array;
		// "object[]" for a List.
		std::string ToString() const;
	};

	// The fields of one element of a List field (e.g. one Animator event). Elements may hold List fields of
	// their own; they never hold entity references (only top-level fields are remapped and resolved by name).
	struct ComponentObjectSchema
	{
		// Type name for tooling (the Lua class in Docs/basalt.d.lua), e.g. "AnimationEvent".
		std::string Name;
		std::vector<std::string> Fields;
		std::map<std::string, ComponentFieldType> FieldTypes;
		std::map<std::string, std::vector<std::string>> EnumOptions;
		// A default-constructed element as JSON: what an inspector's Add button appends. It passes validation.
		nlohmann::json Default;
	};

	// Uniform, name-based access to every serializable component. Scene files, prefabs, editor undo
	// snapshots, Lua (Entity:GetComponent/SetComponent) and the automation API all go through these
	// functions, so a component behaves identically everywhere.
	//
	// Deserialize merges: only keys present in the JSON are changed, and a missing component is added with
	// its defaults and the given keys (one construction, no separate update). Unknown keys, wrong value
	// types and values the component's rules reject fail with a descriptive error, leaving the entity
	// unchanged.
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
		// JSON type of each field, for tooling (component.types, the Lua type definitions).
		std::map<std::string, ComponentFieldType> FieldTypes;
		// Valid values of string-enum fields (e.g. RigidBody "Type"), for inspectors and validation messages.
		std::map<std::string, std::vector<std::string>> EnumOptions;
		// Fields that hold an entity UUID (collected from the field list). Tools may let users name entities
		// there (see component.set), and inspectors show an entity picker.
		std::vector<std::string> EntityFields;
		// Rewrites the component's entity references (e.g. Joint "ConnectedEntity") through the map;
		// references not in the map are kept. Empty for components without EntityFields.
		std::function<void(Entity, const std::unordered_map<UUID, UUID>&)> RemapEntityReferences;
	};

	class ComponentRegistry
	{
	public:
		static const std::vector<ComponentInfo>& GetAll();
		static const ComponentInfo* Find(std::string_view name);
		// Comma-separated list of component names, for error messages.
		static std::string GetNameList();

		// Adds the component if needed and merges the JSON into it; on failure the entity is unchanged.
		static bool AddOrPatch(Entity entity, std::string_view name, const nlohmann::json& data, std::string& outError);

		// Entities copied with fresh UUIDs (prefab instances, duplicated trees) keep references to the
		// originals; this points references to entities in the copied set (old UUID -> new UUID) at the copies.
		static void RemapEntityReferences(Entity entity, const std::unordered_map<UUID, UUID>& remap);
	};

}
