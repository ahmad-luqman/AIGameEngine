#include "Basalt/Core/MouseCodes.h"

#include <cctype>
#include <string>

namespace Basalt {

	namespace {

		constexpr std::string_view s_ButtonNames[MouseButtonCount] = {
			"Left", "Right", "Middle", "Button3", "Button4", "Button5", "Button6", "Button7"
		};

	}

	std::string_view MouseCodeToString(MouseCode button)
	{
		const auto index = static_cast<uint32_t>(button);
		return index < MouseButtonCount ? s_ButtonNames[index] : std::string_view("Unknown");
	}

	std::optional<MouseCode> MouseCodeFromString(std::string_view name)
	{
		for (uint32_t i = 0; i < MouseButtonCount; i++)
		{
			const std::string_view candidate = s_ButtonNames[i];
			if (candidate.size() != name.size())
				continue;

			bool equal = true;
			for (size_t c = 0; c < name.size() && equal; c++)
				equal = std::tolower(static_cast<unsigned char>(candidate[c])) == std::tolower(static_cast<unsigned char>(name[c]));
			if (equal)
				return static_cast<MouseCode>(i);
		}
		return std::nullopt;
	}

}
