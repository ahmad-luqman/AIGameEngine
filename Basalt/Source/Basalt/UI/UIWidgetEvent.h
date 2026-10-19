#pragma once

#include "Basalt/Core/UUID.h"

#include <string>
#include <variant>

namespace Basalt {

	// What a widget callback receives besides the widget: nothing for a click, the new value of a slider (number)
	// or of a checkbox (boolean), a text input (string), or a dropdown (zero-based numeric index).
	using UIWidgetValue = std::variant<std::monostate, float, bool, std::string>;

	// A user interaction with a widget component, for its target's script: Target:Method(widget[, value]). Produced
	// by UIInteraction, dispatched by the ScriptEngine before OnUpdate.
	struct UIWidgetEvent
	{
		UUID Widget = 0;
		// The entity whose script is called: already resolved (the component's Target, or the widget itself when
		// that is 0), never 0.
		UUID Target = 0;
		// Never empty: a widget without a callback name queues no event.
		std::string Method;
		UIWidgetValue Value;
	};

}
