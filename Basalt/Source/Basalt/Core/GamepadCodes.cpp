#include "Basalt/Core/GamepadCodes.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace Basalt {

	namespace {

		constexpr std::array<std::string_view, GamepadButtonCount> s_ButtonNames = {
			"A", "B", "X", "Y", "LeftBumper", "RightBumper", "Back", "Start", "Guide", "LeftThumb", "RightThumb", "DPadUp", "DPadRight", "DPadDown", "DPadLeft"
		};

		constexpr std::array<std::string_view, GamepadAxisCount> s_AxisNames = {
			"LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger"
		};

		// std::array zero-fills missing initializers: every enum value must have a name.
		static_assert(std::ranges::none_of(s_ButtonNames, &std::string_view::empty));
		static_assert(std::ranges::none_of(s_AxisNames, &std::string_view::empty));

		bool EqualsIgnoreCase(std::string_view a, std::string_view b)
		{
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); i++)
			{
				if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
					return false;
			}
			return true;
		}

		template<typename T, size_t N>
		std::optional<T> FindName(const std::array<std::string_view, N>& names, std::string_view name)
		{
			for (size_t i = 0; i < N; i++)
			{
				if (EqualsIgnoreCase(names[i], name))
					return static_cast<T>(i);
			}
			return std::nullopt;
		}

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
		return FindName<GamepadButton>(s_ButtonNames, name);
	}

	std::optional<GamepadAxis> GamepadAxisFromString(std::string_view name)
	{
		return FindName<GamepadAxis>(s_AxisNames, name);
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
