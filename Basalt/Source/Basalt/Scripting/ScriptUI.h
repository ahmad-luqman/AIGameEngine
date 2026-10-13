#pragma once

#include <sol/sol.hpp>

namespace Basalt {

	class ScriptContext;

	// Registers the UI table: immediate-mode UI.Text / UI.Rect / UI.Image with anchors, the immediate-mode widgets
	// UI.Button / UI.Checkbox / UI.Slider, focus (UI.GetFocus / UI.SetFocus / UI.IsPointerOverWidget), UI.GetSize,
	// and UI.GetRect for the rects of UI components (see Docs/ScriptingAPI.md, "Screen UI").
	void RegisterUIBindings(sol::state& lua, ScriptContext& context);

}
