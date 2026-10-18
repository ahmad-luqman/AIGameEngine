#pragma once

#include <Basalt/Scene/ComponentRegistry.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace Basalt {

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
	// index out of range, a move past either end, or growing past type.MaxCount.
	bool EditJsonList(nlohmann::json& array, JsonListEdit edit, size_t index, const ComponentFieldType& type);

	// Draws a List field as a tree: one node per element (labelled by its Name or Clip) holding the element's fields,
	// with move, duplicate and remove controls per element and an Add button. Structural edits change and commit at
	// once; field edits report like DrawJsonField.
	JsonFieldResult DrawJsonList(const std::string& name, nlohmann::json& array, const ComponentFieldType& type);

}
