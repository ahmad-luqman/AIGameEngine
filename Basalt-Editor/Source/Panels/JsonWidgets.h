#pragma once

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
	// picker when the name contains "Color".
	JsonFieldResult DrawJsonField(const std::string& name, nlohmann::json& value, const std::vector<std::string>* enumOptions = nullptr);

}
