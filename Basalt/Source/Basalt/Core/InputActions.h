#pragma once

#include "Basalt/Core/GamepadCodes.h"
#include "Basalt/Core/Input.h"
#include "Basalt/Core/KeyCodes.h"
#include "Basalt/Core/MouseCodes.h"

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Basalt {

	// What an action reports: a button (down or not, with a strength from 0 to 1) or an axis (-1 to 1).
	enum class InputActionType : uint8_t
	{
		Button = 0,
		Axis
	};

	inline constexpr uint32_t InputActionTypeCount = 2;

	// "Button", "Axis"; "Unknown" for values outside the enum.
	std::string_view InputActionTypeToString(InputActionType type);
	// Exact (case-sensitive) inverse of InputActionTypeToString, like the component enums in data files. Control names
	// in bindings are case-insensitive instead, like the key names Lua scripts use.
	std::optional<InputActionType> InputActionTypeFromString(std::string_view name);

	// The JSON keys that name a binding's control, in the order of InputBinding::Control's alternatives.
	inline constexpr std::array<std::string_view, 4> InputControlKeys = { "Key", "MouseButton", "GamepadButton", "GamepadAxis" };

	// One control an action reads, and how its value is shaped.
	struct InputBinding
	{
		static constexpr float DefaultDeadZone = 0.2f;
		static constexpr float MaxDeadZone = 0.95f;

		// Keys and buttons read 1 while held (0 otherwise); a gamepad axis reads its value past the dead zone.
		std::variant<KeyCode, MouseCode, GamepadButton, GamepadAxis> Control = KeyCode::Space;
		// Multiplies the value, in [-1, 1]: -1 makes a key the negative half of an axis, or a stick pushed up (-Y)
		// press a button.
		float Scale = 1.0f;
		// Gamepad axes only, in [0, MaxDeadZone]: a magnitude up to it reads 0, and the rest is rescaled so a stick
		// pushed all the way still reads 1. Sticks rest a little off center, so the default is not 0.
		float DeadZone = DefaultDeadZone;
		// Gamepad controls only: the gamepad to read (local multiplayer), or nullopt for every gamepad (the one
		// pushed furthest wins).
		std::optional<uint32_t> Gamepad;

		bool IsGamepadControl() const { return std::holds_alternative<GamepadButton>(Control) || std::holds_alternative<GamepadAxis>(Control); }

		bool operator==(const InputBinding&) const = default;
	};

	// A named action and the controls bound to it. Inside an InputActionMap the name is 1 to MaxNameLength bytes
	// without control characters and unique, and there are at most MaxBindings bindings.
	struct InputAction
	{
		std::string Name;
		InputActionType Type = InputActionType::Button;
		std::vector<InputBinding> Bindings;

		bool operator==(const InputAction&) const = default;
	};

	// A project's named input actions (the Input.binput file): "Jump" bound to Space and the A button, "MoveX" to
	// A/D, the arrows and the left stick. Scripts read actions rather than keys (Lua Input.GetAction), so players and
	// designers can rebind controls without changing scripts. Actions are evaluated against GameInput: they see only
	// what the host routes to the game.
	//
	// A button's strength is its strongest binding, clamped to [0, 1]; an axis' value is the sum of its bindings,
	// clamped to [-1, 1] (so A and D held together cancel, and a key and the stick do not add past full). Either is
	// down from PressThreshold on (an axis by magnitude); pressed and released compare with the previous frame.
	class InputActionMap
	{
	public:
		static constexpr uint32_t MaxActions = 256;
		static constexpr uint32_t MaxBindings = 16;
		static constexpr size_t MaxNameLength = 64;
		static constexpr float PressThreshold = 0.5f;
		// The file's "Format" and "Version".
		static constexpr const char* FormatName = "BasaltInput";
		static constexpr int FormatVersion = 1;

		const std::vector<InputAction>& GetActions() const { return m_Actions; }
		bool IsEmpty() const { return m_Actions.empty(); }
		const InputAction* Find(std::string_view name) const;

		// Replaces the bindings of an existing action (Lua Input.SetBindings: actions are declared by the project,
		// scripts rebind them). Fails for an unknown action, more than MaxBindings bindings or an invalid one
		// (ValidateBinding); nothing changes then.
		bool SetBindings(std::string_view name, std::vector<InputBinding> bindings, std::string& outError);

		// The action's value on the current or the previous frame: a button's strength (0..1) or an axis' value
		// (-1..1).
		static float GetValue(const InputAction& action, InputFrame frame = InputFrame::Current);
		static bool IsDown(const InputAction& action, InputFrame frame = InputFrame::Current);
		static bool IsPressed(const InputAction& action);
		static bool IsReleased(const InputAction& action);

		// {"Format": "BasaltInput", "Version": 1, "Actions": [...]}, with canonical control names and only the binding
		// fields that differ from their defaults.
		nlohmann::json ToJson() const;
		// The "Actions" array of ToJson: [{"Name", "Type", "Bindings": [...]}, ...].
		nlohmann::json ActionsToJson() const;
		// Strict: rejects unknown keys, another Format or Version (both optional), empty, overlong, duplicate or
		// control-character names, more than MaxActions actions or MaxBindings bindings, and invalid bindings
		// (BindingFromJson). {} is an empty map; an action needs a Name, its Type defaults to Button and its Bindings
		// to none. Errors name the action (by index when its name is the problem).
		static std::optional<InputActionMap> FromJson(const nlohmann::json& data, std::string& outError);

		// {"Key": "Space"} | {"MouseButton": "Left"} | {"GamepadButton": "A"} | {"GamepadAxis": "LeftX"}, plus
		// "Scale", "DeadZone" (gamepad axes) and "Gamepad" (gamepad controls) when they differ from the defaults.
		static nlohmann::json BindingToJson(const InputBinding& binding);
		static nlohmann::json BindingsToJson(const std::vector<InputBinding>& bindings);
		// Strict: exactly one control key with a known control name (case-insensitive), then ValidateBinding.
		static std::optional<InputBinding> BindingFromJson(const nlohmann::json& data, std::string& outError);
		// An array of at most MaxBindings bindings.
		static std::optional<std::vector<InputBinding>> BindingsFromJson(const nlohmann::json& data, std::string& outError);
		// What every binding in a map satisfies: Scale in [-1, 1], DeadZone in [0, MaxDeadZone] and Gamepad an index
		// below Input::MaxGamepads, each finite and only where it applies (a DeadZone other than the default, or a
		// Gamepad, on a key or mouse binding is an error). So ToJson's output always loads again.
		static bool ValidateBinding(const InputBinding& binding, std::string& outError);

		bool operator==(const InputActionMap&) const = default;

	private:
		std::vector<InputAction> m_Actions;
	};

}
