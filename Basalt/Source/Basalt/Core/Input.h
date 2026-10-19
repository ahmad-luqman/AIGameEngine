#pragma once

#include "Basalt/Core/GamepadCodes.h"
#include "Basalt/Core/KeyCodes.h"
#include "Basalt/Core/MouseCodes.h"

#include <glm/vec2.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

	// Which frame a state read describes: the current one, or the previous one (as EndFrame last rolled it over).
	enum class InputFrame : uint8_t
	{
		Current = 0,
		Previous
	};

	// The mouse cursor a game asks for (Input::SetCursorMode). Hosts apply it to their window.
	enum class CursorMode : uint8_t
	{
		// Visible and free.
		Normal = 0,
		// Hidden over the window but free to leave it (a game that draws its own cursor).
		Hidden,
		// Hidden and held in the window: only the mouse delta means anything (first-person cameras).
		Locked
	};

	inline constexpr uint32_t CursorModeCount = 3;

	// "Normal", "Hidden", "Locked"; "Unknown" for values outside the enum.
	std::string_view CursorModeToString(CursorMode mode);
	// Case-insensitive inverse of CursorModeToString.
	std::optional<CursorMode> CursorModeFromString(std::string_view name);

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
	//
	// These reads are the host's: they see every event (the editor's shortcuts and camera use them). The game reads
	// through GameInput, which sees only what the host routes to it (SetGameFocus, SetGamePointer).
	class Input
	{
	public:
		static constexpr uint32_t MaxGamepads = 16;
		static constexpr const char* DefaultInjectedGamepadName = "Injected Gamepad";

		static bool IsKeyDown(KeyCode key, InputFrame frame = InputFrame::Current);
		static bool IsKeyPressed(KeyCode key);
		static bool IsKeyReleased(KeyCode key);

		static bool IsMouseButtonDown(MouseCode button, InputFrame frame = InputFrame::Current);
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
		// IsGamepadButtonReleased still reports the buttons held at a disconnect, on that frame, and the Previous frame
		// of a gamepad disconnected since reads what it held then (here and in GameInput).
		static bool IsGamepadConnected(uint32_t index);
		// An id of the gamepad's current connection, new for every connect (0 while disconnected): when it changes,
		// the gamepad at `index` was disconnected or replaced, even between two looks at it.
		static uint32_t GetGamepadConnection(uint32_t index);
		static GamepadSource GetGamepadSource(uint32_t index);
		// The device's name ("Xbox Controller", ...); empty when not connected.
		static std::string GetGamepadName(uint32_t index);
		static bool IsGamepadButtonDown(uint32_t index, GamepadButton button, InputFrame frame = InputFrame::Current);
		static bool IsGamepadButtonPressed(uint32_t index, GamepadButton button);
		static bool IsGamepadButtonReleased(uint32_t index, GamepadButton button);
		// Sticks -1..1 (+Y down), triggers 0..1.
		static float GetGamepadAxis(uint32_t index, GamepadAxis axis, InputFrame frame = InputFrame::Current);

		// --- Game routing (see GameInput) ------------------------------------------------------
		// Whether the game has the keyboard and gamepads (focus) and the mouse (pointer). Both true until a host says
		// otherwise: the runtime window and headless runs are the game; the editor follows its Game view (focus while
		// the view has keyboard focus, the pointer while the mouse is over it or the game holds the cursor locked).
		static void SetGameFocus(bool focused);
		static bool HasGameFocus();
		static void SetGamePointer(bool enabled);
		static bool HasGamePointer();
		// Withholds a held key or mouse button from the game until it is let go, as if it had been held when the
		// routing came back: the host used it (the editor's Escape that releases the cursor, the click that takes it
		// back). Called before the scene update, the game never sees it pressed. Nothing happens for one not held.
		static void WithholdFromGame(KeyCode key);
		static void WithholdFromGame(MouseCode button);

		// The cursor the game asks for (Lua Input.SetCursorMode). Only stored here: the runtime applies it to its
		// window, the editor while its Game view hosts the game with focus, headless runs never. Play sessions start
		// and end with Normal; Reset keeps it.
		static void SetCursorMode(CursorMode mode);
		static CursorMode GetCursorMode();

		// --- State feeding ---------------------------------------------------------------------
		// Updates state from a platform event. Does not mark the event handled.
		static void OnEvent(Event& event);
		// Rolls "pressed/released this frame" into history. Call once at the end of each frame.
		static void EndFrame();
		// Clears every key and button and releases gamepads (e.g. on focus loss or when leaving play mode), for the game
		// too, without reporting releases. Keeps the mouse position, gamepad connections, routing, the cursor mode and
		// settings.
		static void Reset();

		static void SetKeyState(KeyCode key, bool down);
		static void SetMouseButtonState(MouseCode button, bool down);
		static void SetMousePosition(const glm::vec2& position);
		static void AddMouseScroll(const glm::vec2& scroll);
		// Unicode text typed this frame; bounded to 16384 UTF-8 bytes and cleared by EndFrame/Reset.
		// Returns false for C0 (U+0000..001F), DEL (U+007F), invalid scalars or a full frame buffer.
		static bool AddTextCodepoint(uint32_t codepoint);
		static std::string GetTextInput();

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

	// Input as the game sees it (Lua's Input, input actions): only what the host routes to it. The keyboard and
	// gamepads reach the game while it has focus (Input::SetGameFocus), the mouse buttons, movement and scroll while
	// it has the pointer (Input::SetGamePointer); the mouse position, gamepad connections and names are never
	// withheld. Losing the routing reads like letting go of everything held (released on that frame); a key or button
	// held when the routing comes back stays released for the game until it is let go (a stick until it returns to
	// rest). The editor gives the game the pointer only while its Game view also has focus, so the click that focuses
	// the view is not the game's. With full routing (the runtime, headless runs) every read equals Input's.
	class GameInput
	{
	public:
		static std::string GetTextInput();

		static bool IsKeyDown(KeyCode key, InputFrame frame = InputFrame::Current);
		static bool IsKeyPressed(KeyCode key);
		static bool IsKeyReleased(KeyCode key);

		static bool IsMouseButtonDown(MouseCode button, InputFrame frame = InputFrame::Current);
		static bool IsMouseButtonPressed(MouseCode button);
		static bool IsMouseButtonReleased(MouseCode button);
		// Zero while the game does not have the pointer.
		static glm::vec2 GetMouseDelta();
		static glm::vec2 GetMouseScroll();

		static bool IsGamepadButtonDown(uint32_t index, GamepadButton button, InputFrame frame = InputFrame::Current);
		static bool IsGamepadButtonPressed(uint32_t index, GamepadButton button);
		static bool IsGamepadButtonReleased(uint32_t index, GamepadButton button);
		static float GetGamepadAxis(uint32_t index, GamepadAxis axis, InputFrame frame = InputFrame::Current);
	};

}
