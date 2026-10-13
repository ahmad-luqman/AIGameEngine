#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace Basalt {

	// Buttons of a gamepad in the standard (Xbox-style) layout. Values match GLFW's gamepad button indices, so
	// GLFWgamepadstate converts without a lookup table; GLFW's SDL mapping database maps other controllers onto it.
	enum class GamepadButton : uint8_t
	{
		A = 0, // bottom face button (Cross on PlayStation)
		B,     // right face button (Circle)
		X,     // left face button (Square)
		Y,     // top face button (Triangle)
		LeftBumper,
		RightBumper,
		Back,
		Start,
		Guide,
		LeftThumb,
		RightThumb,
		DPadUp,
		DPadRight,
		DPadDown,
		DPadLeft
	};

	inline constexpr uint32_t GamepadButtonCount = 15;
	static_assert(static_cast<uint32_t>(GamepadButton::DPadLeft) + 1 == GamepadButtonCount);

	// Axes of a gamepad in the standard layout (GLFW's gamepad axis indices). Sticks go from -1 to 1 with +X right and
	// +Y down; triggers from 0 (released) to 1.
	enum class GamepadAxis : uint8_t
	{
		LeftX = 0,
		LeftY,
		RightX,
		RightY,
		LeftTrigger,
		RightTrigger
	};

	inline constexpr uint32_t GamepadAxisCount = 6;
	static_assert(static_cast<uint32_t>(GamepadAxis::RightTrigger) + 1 == GamepadAxisCount);

	// Canonical names ("A", "DPadUp", "LeftX", ...); "Unknown" for values outside the enum.
	std::string_view GamepadButtonToString(GamepadButton button);
	std::string_view GamepadAxisToString(GamepadAxis axis);
	// Case-insensitive inverses of the above.
	std::optional<GamepadButton> GamepadButtonFromString(std::string_view name);
	std::optional<GamepadAxis> GamepadAxisFromString(std::string_view name);
	// Every name, in enum order (for error messages and documentation).
	std::span<const std::string_view> GetGamepadButtonNames();
	std::span<const std::string_view> GetGamepadAxisNames();

	// Whether a trigger axis (0..1) rather than a stick axis (-1..1).
	constexpr bool IsGamepadTrigger(GamepadAxis axis)
	{
		return axis == GamepadAxis::LeftTrigger || axis == GamepadAxis::RightTrigger;
	}

}
