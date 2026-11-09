#pragma once

#include "Breccia/Scene/ComponentRegistry.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Breccia {

	class Entity;
	class Scene;

	struct JsonFieldResult
	{
		// The value was modified this frame.
		bool Changed = false;
		// An edit finished this frame (slider released, text committed): time to record undo history.
		bool Committed = false;
	};

	// Draws an editor widget for one JSON-typed field and edits the value in place. The widget is chosen
	// from the value's type and the field name: numbers -> drag, bools -> checkbox, strings -> text (or a
	// combo when enumOptions is given, and an asset drop target), [x,y,z(,w)] -> vector drag, or a color
	// picker when the name contains "Color"; three booleans -> per-axis checkboxes, three strings with
	// enumOptions -> per-axis combos.
	JsonFieldResult DrawJsonField(const std::string& name, nlohmann::json& value, const std::vector<std::string>* enumOptions = nullptr);

	// Structural edits of a List field (ComponentFieldKind::List) value.
	enum class JsonListEdit : uint8_t
	{
		Add,       // appends the element schema's default (index unused)
		Duplicate, // inserts a copy after the element
		Remove,
		MoveUp,
		MoveDown
	};

	// Applies one edit to a JSON array of `type`'s elements; false (array unchanged) when it does not apply: an
	// index out of range, a move past either end, growing past type.MaxCount, or no representable free time. An added or duplicated element
	// gets a fresh value of the schema's UniqueField ("Walk 2"). Timed lists insert into a free interval;
	// Add uses time 0 for the first key, appends time 1 when possible, otherwise splits the widest interval.
	// Duplicate prefers the following interval, falling back to the widest representable gap.
	// insertedIndex receives the actual new slot so tree state follows it. Moves retain the ordered times.
	bool EditJsonList(nlohmann::json& array, JsonListEdit edit, size_t index, const ComponentFieldType& type, size_t* insertedIndex = nullptr);

	// Draws a List field as a tree: one node per element (labelled by its Name or Clip) holding the element's fields,
	// with move and remove buttons per element (duplicate and the rest in its context menu) and an Add button.
	// Structural edits change and commit at once; field edits report like DrawJsonField. `scene` lists the entities entity
	// fields in the elements can name.
	JsonFieldResult DrawJsonList(const std::string& name, nlohmann::json& array, const ComponentFieldType& type, Scene& scene);

	// One field of a component or list element: DrawJsonList for a List field, DrawEntityPicker for a list element's entity
	// field (0 is the entity holding the list), DrawJsonField (with the field's enum options) for anything else. A
	// component's own entity fields each decide what 0 means, so their callers draw the picker themselves.
	JsonFieldResult DrawJsonValue(const std::string& name, nlohmann::json& value, const ComponentFieldType& type, const std::map<std::string, std::vector<std::string>>& enumOptions, Scene& scene);

	// What 0 means in a component's own entity field, as the picker's label for it: the entity itself for a joint's BodyEntity
	// and a widget's Target, the world for a joint's ConnectedEntity, the parent for a BoneAttachment's Target and spatial
	// navigation for UINavigation's links.
	const char* EntityReferenceNone(const std::string& component, const std::string& field);

	// A combo of the scene's entities `filter` accepts, labelled `name` and showing `current` (0 as noneLabel; an entity
	// that is gone as missing, so it can be replaced). Returns the ID picked this frame, if it differs.
	std::optional<uint64_t> DrawEntityPicker(Scene& scene, const std::string& name, uint64_t current, const std::function<bool(Entity)>& filter, const char* noneLabel);

}
