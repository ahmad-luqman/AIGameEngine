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

	// Who drives a gamepad slot.
	enum class GamepadSource : uint8_t
	{
		// Nothing is connected.
		None = 0,
		// A physical gamepad, polled by the window host.
		Device,
		// Injected by tests or the automation API; device polls leave it alone.
		Injected
	};

	// Polled input state. Fed by window events (or injected by tests / the automation API), so it works
	// identically with and without a window.
	//
	// "Down" means held. "Pressed"/"Released" are true only on the frame the transition happened.
	//
	// Gamepads are indexed 0..MaxGamepads-1 (GLFW joystick ids). A window host polls the devices every frame
	// (Window::PollEvents); tests and the automation API inject gamepads instead, and the device poll leaves an
	// injected gamepad alone until it is disconnected again. A reset (focus loss) releases every button and
	// centers every stick. Physical gamepads read as released while the window is unfocused, and a button or stick
	// held through a reset or an unfocused stretch stays released until it is let go, so it does not press again.
	class Input
	{
	public:
		static constexpr uint32_t MaxGamepads = 16;
		static constexpr const char* DefaultInjectedGamepadName = "Injected Gamepad";

		static bool IsKeyDown(KeyCode key);
		static bool IsKeyPressed(KeyCode key);
		static bool IsKeyReleased(KeyCode key);

		static bool IsMouseButtonDown(MouseCode button);
		static bool IsMouseButtonPressed(MouseCode button);
		static bool IsMouseButtonReleased(MouseCode button);

		static glm::vec2 GetMousePosition();
		// Whether any mouse position arrived yet (before that, GetMousePosition is 0,0 but means nothing).
		static bool HasMousePosition();
		// Forgets the mouse position, as before the first one arrived (a replay starts without one).
		static void ResetMousePosition();
		// Mouse movement since the previous frame.
		static glm::vec2 GetMouseDelta();
		// Scroll accumulated during the current frame.
		static glm::vec2 GetMouseScroll();

		// Out-of-range indices and disconnected gamepads read as buttons up and centered axes;
		// IsGamepadButtonReleased still reports the buttons held at a disconnect, on that frame.
		static bool IsGamepadConnected(uint32_t index);
		// An id of the gamepad's current connection, new for every connect (0 while disconnected): when it changes,
		// the gamepad at `index` was disconnected or replaced, even between two looks at it.
		static uint32_t GetGamepadConnection(uint32_t index);
		static GamepadSource GetGamepadSource(uint32_t index);
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
		// Clears every key and button and releases gamepads (e.g. on focus loss or when leaving play mode). Keeps the
		// mouse position, gamepad connections and settings.
		static void Reset();

		static void SetKeyState(KeyCode key, bool down);
		static void SetMouseButtonState(MouseCode button, bool down);
		static void SetMousePosition(const glm::vec2& position);
		static void AddMouseScroll(const glm::vec2& scroll);

		// Injected gamepads (tests, automation). Connecting names the gamepad and makes it injected (taking over a
		// physical gamepad at that index, released); disconnecting releases everything (a physical gamepad there
		// comes back with the next poll). The button and axis setters return false for an index out of range or a
		// gamepad that is not injected. Axis values are clamped to the axis' range; NaN centers it.
		static void SetGamepadConnected(uint32_t index, bool connected, const std::string& name = DefaultInjectedGamepadName);
		static bool SetGamepadButtonState(uint32_t index, GamepadButton button, bool down);
		static bool SetGamepadAxis(uint32_t index, GamepadAxis axis, float value);
		// Removes every injected gamepad as if it had never been connected (no release is reported; a replay starts
		// from no gamepads).
		static void ForgetInjectedGamepads();
		// While disabled, physical gamepads are forgotten like ForgetInjectedGamepads and device polls are ignored, so
		// replays see only the gamepads they inject. Enabled by default; Reset keeps the setting.
		static void SetDeviceGamepadsEnabled(bool enabled);
		static bool IsDeviceGamepadsEnabled();
		// A device poll: `state` is the device's current state, or nullptr when no gamepad is at `index`; `focused`
		// whether the window has focus (unfocused, the gamepad reads as released and what it holds is blocked until
		// let go). Ignored for injected gamepads and while device gamepads are disabled.
		static void UpdateGamepadFromDevice(uint32_t index, const GamepadDeviceState* state, bool focused = true);
	};

}
