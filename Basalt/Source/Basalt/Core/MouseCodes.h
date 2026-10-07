#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace Basalt {

	// Values match GLFW mouse button indices.
	enum class MouseCode : uint8_t
	{
		Left = 0,
		Right = 1,
		Middle = 2,
		Button3 = 3,
		Button4 = 4,
		Button5 = 5,
		Button6 = 6,
		Button7 = 7
	};

	inline constexpr uint32_t MouseButtonCount = 8;

	std::string_view MouseCodeToString(MouseCode button);
	std::optional<MouseCode> MouseCodeFromString(std::string_view name);

}
