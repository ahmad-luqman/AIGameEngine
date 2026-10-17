#include "Basalt/Core/MouseCodes.h"

#include "Basalt/Core/EnumNames.h"

namespace Basalt {

	namespace {

		constexpr std::string_view ButtonNames[MouseButtonCount] = {
			"Left", "Right", "Middle", "Button3", "Button4", "Button5", "Button6", "Button7"
		};

	}

	std::string_view MouseCodeToString(MouseCode button)
	{
		const auto index = static_cast<uint32_t>(button);
		return index < MouseButtonCount ? ButtonNames[index] : std::string_view("Unknown");
	}

	std::span<const std::string_view> GetMouseButtonNames()
	{
		return ButtonNames;
	}

	std::optional<MouseCode> MouseCodeFromString(std::string_view name)
	{
		return EnumFromNameIgnoreCase<MouseCode>(ButtonNames, name);
	}

}
