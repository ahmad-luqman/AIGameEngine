#pragma once

#include <sol/sol.hpp>

namespace Basalt {

	// Registers the UI table: immediate-mode UI.Text / UI.Rect / UI.Image with anchors, UI.GetSize, and
	// UI.GetRect for the rects of UI components (see Docs/ScriptingAPI.md, "Screen UI").
	void RegisterUIBindings(sol::state& lua);

}
