#pragma once

#include "Basalt/Scene/Components.h"

#include <string>

namespace Basalt {

	// The value rules of the UI components, shared by the two places that apply them: the component registry
	// rejects values outside them (files, Lua, automation, inspector) and the UI layout repairs components
	// that C++ code wrote directly. One walk over the rules does both, so a component is valid exactly when
	// its Sanitize function leaves it unchanged.

	// The first rule the component breaks, naming the field and its value; empty when it is valid.
	std::string ValidateUIRect(const UIRectComponent& rect);
	std::string ValidateUIImage(const UIImageComponent& image);
	std::string ValidateUIText(const UITextComponent& text);

	// The component with every value moved into its range: non-finite values and out-of-range enums take the
	// default component's value, everything else is clamped (AnchorMax is raised to AnchorMin).
	UIRectComponent SanitizeUIRect(UIRectComponent rect);
	UIImageComponent SanitizeUIImage(UIImageComponent image);
	UITextComponent SanitizeUIText(UITextComponent text);

}
