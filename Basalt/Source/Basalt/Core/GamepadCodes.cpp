#include "Basalt/Core/GamepadCodes.h"

#include "Basalt/Core/EnumNames.h"

#include <algorithm>
#include <array>

namespace Basalt {

	namespace {

		constexpr std::array<std::string_view, GamepadButtonCount> s_ButtonNames = {
			"A", "B", "X", "Y", "LeftBumper", "RightBumper", "Back", "Start", "Guide", "LeftThumb", "RightThumb", "DPadUp", "DPadRight", "DPadDown", "DPadLeft"
		};

		constexpr std::array<std::string_view, GamepadAxisCount> s_AxisNames = {
			"LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger"
		};

		// std::array zero-fills missing initializers: every enum value must have a name.
		constexpr bool IsEmpty(std::string_view name)
		{
			return name.empty();
		}
		static_assert(std::ranges::none_of(s_ButtonNames, IsEmpty));
		static_assert(std::ranges::none_of(s_AxisNames, IsEmpty));

	}

	std::string_view GamepadButtonToString(GamepadButton button)
	{
		const auto index = static_cast<uint32_t>(button);
		return index < GamepadButtonCount ? s_ButtonNames[index] : std::string_view("Unknown");
	}

	std::string_view GamepadAxisToString(GamepadAxis axis)
	{
		const auto index = static_cast<uint32_t>(axis);
		return index < GamepadAxisCount ? s_AxisNames[index] : std::string_view("Unknown");
	}

	std::optional<GamepadButton> GamepadButtonFromString(std::string_view name)
	{
		return EnumFromNameIgnoreCase<GamepadButton>(s_ButtonNames, name);
	}

	std::optional<GamepadAxis> GamepadAxisFromString(std::string_view name)
	{
		return EnumFromNameIgnoreCase<GamepadAxis>(s_AxisNames, name);
	}

	std::span<const std::string_view> GetGamepadButtonNames()
	{
		return s_ButtonNames;
	}

	std::span<const std::string_view> GetGamepadAxisNames()
	{
		return s_AxisNames;
	}

}
