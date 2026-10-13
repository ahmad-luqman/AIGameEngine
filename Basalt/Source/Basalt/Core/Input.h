#pragma once

#include "Basalt/Core/GamepadCodes.h"
#include "Basalt/Core/KeyCodes.h"
#include "Basalt/Core/MouseCodes.h"

#include <glm/vec2.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace Basalt {

	class Event;

	// One poll of a gamepad device: buttons by GamepadButton, axes by GamepadAxis (sticks -1..1, triggers 0..1).
	struct GamepadDeviceState
	{
		std::string Name;
		std::array<bool, GamepadButtonCount> Buttons = {};
		std::array<float, GamepadAxisCount> Axes = {};
	};

	// Polled input state. Fed by window events (or injected by tests / the automation API), so it works
	// identically with and without a window.
	//
	// "Down" means held. "Pressed"/"Released" are true only on the frame the transition happened.
	//
	// Gamepads are indexed 0..MaxGamepads-1 (GLFW joystick ids). A window host polls the devices every frame
	// (Window::PollEvents); tests and the automation API inject gamepads instead, and the device poll leaves an
	// injected gamepad alone until it is disconnected again.
	class Input
	{
	public:
		static constexpr uint32_t MaxGamepads = 16;

		static bool IsKeyDown(KeyCode key);
		static bool IsKeyPressed(KeyCode key);
		static bool IsKeyReleased(KeyCode key);

		static bool IsMouseButtonDown(MouseCode button);
		static bool IsMouseButtonPressed(MouseCode button);
		static bool IsMouseButtonReleased(MouseCode button);

		static glm::vec2 GetMousePosition();
		// Mouse movement since the previous frame.
		static glm::vec2 GetMouseDelta();
		// Scroll accumulated during the current frame.
		static glm::vec2 GetMouseScroll();

		// Out-of-range indices and disconnected gamepads read as released buttons and centered axes.
		static bool IsGamepadConnected(uint32_t index);
		// Connected or disconnected on the current frame.
		static bool WasGamepadConnected(uint32_t index);
		static bool WasGamepadDisconnected(uint32_t index);
		// The device's name ("Xbox Controller", ...); empty when not connected.
		static std::string GetGamepadName(uint32_t index);
		static bool IsGamepadButtonDown(uint32_t index, GamepadButton button);
		static bool IsGamepadButtonPressed(uint32_t index, GamepadButton button);
		static bool IsGamepadButtonReleased(uint32_t index, GamepadButton button);
		// Sticks -1..1 (+Y down), triggers 0..1.
		static float GetGamepadAxis(uint32_t index, GamepadAxis axis);

		// --- State feeding ---------------------------------------------------------------------
		// Updates state from a platform event. Does not mark the event handled.
		static void OnEvent(Event& event);
		// Rolls "pressed/released this frame" into history. Call once at the end of each frame.
		static void EndFrame();
		// Clears every key and button (e.g. on focus loss or when leaving play mode).
		static void Reset();

		static void SetKeyState(KeyCode key, bool down);
		static void SetMouseButtonState(MouseCode button, bool down);
		static void SetMousePosition(const glm::vec2& position);
		static void AddMouseScroll(const glm::vec2& scroll);

		// Injected gamepads (tests, automation). Connecting names the gamepad; disconnecting releases everything.
		// The button and axis setters return false for an index out of range or a gamepad that is not connected.
		// Axis values are clamped to the axis' range; NaN centers it.
		static void SetGamepadConnected(uint32_t index, bool connected, const std::string& name = "Injected Gamepad");
		static bool SetGamepadButtonState(uint32_t index, GamepadButton button, bool down);
		static bool SetGamepadAxis(uint32_t index, GamepadAxis axis, float value);
		// Removes every injected gamepad as if it had never been connected: no disconnect is reported, and a gamepad
		// connected again on the same frame reports its connection (a replay starts from no gamepads).
		static void ForgetInjectedGamepads();
		// A device poll: `state` is the device's current state, or nullptr when no gamepad is at `index`. Ignored
		// for injected gamepads.
		static void UpdateGamepadFromDevice(uint32_t index, const GamepadDeviceState* state);
	};

}
