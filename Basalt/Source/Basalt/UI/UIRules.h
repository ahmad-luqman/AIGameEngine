#pragma once

#include "Basalt/Scene/Components.h"

#include <string>

namespace Basalt {

	// The value rules of the UI components: the component registry rejects values outside them (files, Lua,
	// automation, inspector), and the UI layout and editor repair components that C++ code wrote directly. One walk over the rules does both, so a component is valid exactly when
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

	// The value a rule gives `value`: `fallback` when it is not finite, otherwise clamped to [min, max].
	float ClampOrFallback(float value, float min, float max, float fallback);

	// Where text sits in the free space of its rect: 0 (Left/Top), 0.5 (Center/Middle) or 1 (Right/Bottom).
	float UIAlignFraction(UIHorizontalAlign align);
	float UIAlignFraction(UIVerticalAlign align);

}
