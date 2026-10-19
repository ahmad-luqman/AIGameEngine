#include "Basalt/Core/Input.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/EnumNames.h"
#include "Basalt/Events/ApplicationEvent.h"
#include "Basalt/Events/KeyEvent.h"
#include "Basalt/Events/MouseEvent.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace Basalt {

	namespace {

		std::string s_TextInput;
		std::string s_GameTextInput;

		constexpr size_t MaxKeys = 512;

		// A stick or trigger counts as let go below this (see GamepadSlot::ButtonBlocked).
		constexpr float AxisRestThreshold = 0.25f;

		bool IsAxisPushed(float value)
		{
			return std::abs(value) >= AxisRestThreshold;
		}

		struct GamepadSlot
		{
			GamepadSource Source = GamepadSource::None;
			// Input::GetGamepadConnection: new for every connect, 0 while disconnected.
			uint32_t Connection = 0;
			std::string Name;
			std::array<bool, GamepadButtonCount> Buttons = {};
			std::array<bool, GamepadButtonCount> ButtonsLastFrame = {};
			std::array<float, GamepadAxisCount> Axes = {};
			std::array<float, GamepadAxisCount> AxesLastFrame = {};
			// A physical button or axis held through a reset or while the window was unfocused: it reads as released
			// until the device lets it go, so it does not press again by itself.
			std::array<bool, GamepadButtonCount> ButtonBlocked = {};
			std::array<bool, GamepadAxisCount> AxisBlocked = {};
			// The game's view (GameInput): what was held when the game got focus back reads as released for the game
			// until it is let go (or the stick returns to rest), and what the game saw at the last EndFrame.
			std::array<bool, GamepadButtonCount> GameButtonBlocked = {};
			std::array<bool, GamepadAxisCount> GameAxisBlocked = {};
			std::array<bool, GamepadButtonCount> GameButtonsLastFrame = {};
			std::array<float, GamepadAxisCount> GameAxesLastFrame = {};

			bool IsConnected() const { return Source != GamepadSource::None; }

			void ClearInput()
			{
				Buttons = {};
				Axes = {};
				ButtonBlocked = {};
				AxisBlocked = {};
				GameButtonBlocked = {};
				GameAxisBlocked = {};
			}

			// Blocks what is held now (see ButtonBlocked).
			void BlockHeld()
			{
				for (size_t b = 0; b < GamepadButtonCount; b++)
					ButtonBlocked[b] = ButtonBlocked[b] || Buttons[b];
				for (size_t a = 0; a < GamepadAxisCount; a++)
					AxisBlocked[a] = AxisBlocked[a] || IsAxisPushed(Axes[a]);
			}

			// Blocks what is held now for the game only (see GameButtonBlocked).
			void BlockHeldForGame()
			{
				for (size_t b = 0; b < GamepadButtonCount; b++)
					GameButtonBlocked[b] = GameButtonBlocked[b] || Buttons[b];
				for (size_t a = 0; a < GamepadAxisCount; a++)
					GameAxisBlocked[a] = GameAxisBlocked[a] || IsAxisPushed(Axes[a]);
			}

			// Forgets what the game saw, without reporting releases (Input::Reset).
			void ClearGameView()
			{
				GameButtonBlocked = {};
				GameAxisBlocked = {};
				GameButtonsLastFrame = {};
				GameAxesLastFrame = {};
			}

			// Ends the game's blocks of what has been let go.
			void ReleaseGameBlocks()
			{
				for (size_t b = 0; b < GamepadButtonCount; b++)
					GameButtonBlocked[b] = GameButtonBlocked[b] && Buttons[b];
				for (size_t a = 0; a < GamepadAxisCount; a++)
					GameAxisBlocked[a] = GameAxisBlocked[a] && IsAxisPushed(Axes[a]);
			}

			// Keeps the last-frame state, so the buttons held report their release on this frame.
			void Disconnect()
			{
				ClearInput();
				Source = GamepadSource::None;
				Connection = 0;
				Name.clear();
			}
		};

		struct InputState
		{
			std::array<bool, MaxKeys> KeysDown = {};
			std::array<bool, MaxKeys> KeysDownLastFrame = {};
			std::array<bool, MouseButtonCount> ButtonsDown = {};
			std::array<bool, MouseButtonCount> ButtonsDownLastFrame = {};
			glm::vec2 MousePosition = { 0.0f, 0.0f };
			glm::vec2 MousePositionLastFrame = { 0.0f, 0.0f };
			glm::vec2 Scroll = { 0.0f, 0.0f };
			bool HasMousePosition = false;
			std::array<GamepadSlot, Input::MaxGamepads> Gamepads = {};
			// The game's view of the keys and mouse buttons (see GamepadSlot::GameButtonBlocked).
			std::array<bool, MaxKeys> GameKeysBlocked = {};
			std::array<bool, MaxKeys> GameKeysLastFrame = {};
			std::array<bool, MouseButtonCount> GameButtonsBlocked = {};
			std::array<bool, MouseButtonCount> GameButtonsLastFrame = {};
		};

		// All guarded by s_Mutex. The settings (gamepad devices, routing, the cursor mode) outlive Reset, which only
		// clears what is held.
		std::mutex s_Mutex;
		InputState s_State;
		bool s_DeviceGamepadsEnabled = true;
		uint32_t s_NextGamepadConnection = 1;
		bool s_GameFocus = true;
		bool s_GamePointer = true;
		CursorMode s_CursorMode = CursorMode::Normal;

		constexpr std::string_view CursorModeNames[CursorModeCount] = { "Normal", "Hidden", "Locked" };

		size_t KeyIndex(KeyCode key)
		{
			return static_cast<size_t>(key);
		}
		size_t ButtonIndex(MouseCode button)
		{
			return static_cast<size_t>(button);
		}

		// The connected gamepad at `index`, or nullptr. Callers hold s_Mutex.
		GamepadSlot* ConnectedGamepad(uint32_t index)
		{
			if (index >= Input::MaxGamepads || !s_State.Gamepads[index].IsConnected())
				return nullptr;
			return &s_State.Gamepads[index];
		}

		// A new connection id for a slot that becomes connected. Callers hold s_Mutex.
		uint32_t NewConnection()
		{
			const uint32_t connection = s_NextGamepadConnection++;
			if (s_NextGamepadConnection == 0)
				s_NextGamepadConnection = 1;
			return connection;
		}

		// Forgets the gamepads of `source` as if they had never been connected. Callers hold s_Mutex.
		void ForgetGamepads(GamepadSource source)
		{
			for (GamepadSlot& gamepad : s_State.Gamepads)
			{
				if (gamepad.Source == source)
					gamepad = {};
			}
		}

		float ClampAxis(GamepadAxis axis, float value)
		{
			if (std::isnan(value))
				return 0.0f;
			return std::clamp(value, IsGamepadTrigger(axis) ? 0.0f : -1.0f, 1.0f);
		}

		// The game's current view (GameInput). Callers hold s_Mutex and check the index.
		bool GameKeyDown(size_t index)
		{
			return s_GameFocus && s_State.KeysDown[index] && !s_State.GameKeysBlocked[index];
		}
		bool GameButtonDown(size_t index)
		{
			return s_GamePointer && s_State.ButtonsDown[index] && !s_State.GameButtonsBlocked[index];
		}
		bool GameGamepadButtonDown(const GamepadSlot& gamepad, size_t button)
		{
			return s_GameFocus && gamepad.Buttons[button] && !gamepad.GameButtonBlocked[button];
		}
		float GameGamepadAxis(const GamepadSlot& gamepad, size_t axis)
		{
			return s_GameFocus && !gamepad.GameAxisBlocked[axis] ? gamepad.Axes[axis] : 0.0f;
		}

	}

	std::string_view CursorModeToString(CursorMode mode)
	{
		const auto index = static_cast<uint32_t>(mode);
		return index < CursorModeCount ? CursorModeNames[index] : std::string_view("Unknown");
	}

	std::optional<CursorMode> CursorModeFromString(std::string_view name)
	{
		return EnumFromNameIgnoreCase<CursorMode>(CursorModeNames, name);
	}

	bool Input::IsKeyDown(KeyCode key, InputFrame frame)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = KeyIndex(key);
		return index < MaxKeys && (frame == InputFrame::Current ? s_State.KeysDown : s_State.KeysDownLastFrame)[index];
	}

	bool Input::IsKeyPressed(KeyCode key)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = KeyIndex(key);
		return index < MaxKeys && s_State.KeysDown[index] && !s_State.KeysDownLastFrame[index];
	}

	bool Input::IsKeyReleased(KeyCode key)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = KeyIndex(key);
		return index < MaxKeys && !s_State.KeysDown[index] && s_State.KeysDownLastFrame[index];
	}

	bool Input::IsMouseButtonDown(MouseCode button, InputFrame frame)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = ButtonIndex(button);
		return index < MouseButtonCount && (frame == InputFrame::Current ? s_State.ButtonsDown : s_State.ButtonsDownLastFrame)[index];
	}

	bool Input::IsMouseButtonPressed(MouseCode button)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = ButtonIndex(button);
		return index < MouseButtonCount && s_State.ButtonsDown[index] && !s_State.ButtonsDownLastFrame[index];
	}

	bool Input::IsMouseButtonReleased(MouseCode button)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = ButtonIndex(button);
		return index < MouseButtonCount && !s_State.ButtonsDown[index] && s_State.ButtonsDownLastFrame[index];
	}

	glm::vec2 Input::GetMousePosition()
	{
		std::scoped_lock lock(s_Mutex);
		return s_State.MousePosition;
	}

	bool Input::HasMousePosition()
	{
		std::scoped_lock lock(s_Mutex);
		return s_State.HasMousePosition;
	}

	void Input::ResetMousePosition()
	{
		std::scoped_lock lock(s_Mutex);
		s_State.MousePosition = { 0.0f, 0.0f };
		s_State.MousePositionLastFrame = { 0.0f, 0.0f };
		s_State.HasMousePosition = false;
	}

	glm::vec2 Input::GetMouseDelta()
	{
		std::scoped_lock lock(s_Mutex);
		return s_State.MousePosition - s_State.MousePositionLastFrame;
	}

	glm::vec2 Input::GetMouseScroll()
	{
		std::scoped_lock lock(s_Mutex);
		return s_State.Scroll;
	}

	bool Input::IsGamepadConnected(uint32_t index)
	{
		std::scoped_lock lock(s_Mutex);
		return ConnectedGamepad(index) != nullptr;
	}

	uint32_t Input::GetGamepadConnection(uint32_t index)
	{
		std::scoped_lock lock(s_Mutex);
		return index < MaxGamepads ? s_State.Gamepads[index].Connection : 0;
	}

	GamepadSource Input::GetGamepadSource(uint32_t index)
	{
		std::scoped_lock lock(s_Mutex);
		return index < MaxGamepads ? s_State.Gamepads[index].Source : GamepadSource::None;
	}

	std::string Input::GetGamepadName(uint32_t index)
	{
		std::scoped_lock lock(s_Mutex);
		const GamepadSlot* gamepad = ConnectedGamepad(index);
		return gamepad ? gamepad->Name : std::string();
	}

	bool Input::IsGamepadButtonDown(uint32_t index, GamepadButton button, InputFrame frame)
	{
		std::scoped_lock lock(s_Mutex);
		const auto b = static_cast<size_t>(button);
		if (b >= GamepadButtonCount)
			return false;
		if (frame == InputFrame::Previous)
			return index < MaxGamepads && s_State.Gamepads[index].ButtonsLastFrame[b];
		const GamepadSlot* gamepad = ConnectedGamepad(index);
		return gamepad && gamepad->Buttons[b];
	}

	bool Input::IsGamepadButtonPressed(uint32_t index, GamepadButton button)
	{
		std::scoped_lock lock(s_Mutex);
		const GamepadSlot* gamepad = ConnectedGamepad(index);
		const auto b = static_cast<size_t>(button);
		return gamepad && b < GamepadButtonCount && gamepad->Buttons[b] && !gamepad->ButtonsLastFrame[b];
	}

	bool Input::IsGamepadButtonReleased(uint32_t index, GamepadButton button)
	{
		std::scoped_lock lock(s_Mutex);
		// A gamepad disconnected while a button was held reports the release on that frame.
		if (index >= MaxGamepads)
			return false;
		const GamepadSlot& gamepad = s_State.Gamepads[index];
		const auto b = static_cast<size_t>(button);
		return b < GamepadButtonCount && !gamepad.Buttons[b] && gamepad.ButtonsLastFrame[b];
	}

	float Input::GetGamepadAxis(uint32_t index, GamepadAxis axis, InputFrame frame)
	{
		std::scoped_lock lock(s_Mutex);
		const auto a = static_cast<size_t>(axis);
		if (a >= GamepadAxisCount)
			return 0.0f;
		if (frame == InputFrame::Previous)
			return index < MaxGamepads ? s_State.Gamepads[index].AxesLastFrame[a] : 0.0f;
		const GamepadSlot* gamepad = ConnectedGamepad(index);
		return gamepad ? gamepad->Axes[a] : 0.0f;
	}

	void Input::SetGameFocus(bool focused)
	{
		std::scoped_lock lock(s_Mutex);
		// What is held as the game gets focus back is not the game's: it stays released for the game until let go.
		if (focused && !s_GameFocus)
		{
			for (size_t k = 0; k < MaxKeys; k++)
				s_State.GameKeysBlocked[k] = s_State.GameKeysBlocked[k] || s_State.KeysDown[k];
			for (GamepadSlot& gamepad : s_State.Gamepads)
				gamepad.BlockHeldForGame();
		}
		s_GameFocus = focused;
		s_GameTextInput.clear();
	}

	bool Input::HasGameFocus()
	{
		std::scoped_lock lock(s_Mutex);
		return s_GameFocus;
	}

	void Input::SetGamePointer(bool enabled)
	{
		std::scoped_lock lock(s_Mutex);
		if (enabled && !s_GamePointer)
		{
			for (size_t b = 0; b < MouseButtonCount; b++)
				s_State.GameButtonsBlocked[b] = s_State.GameButtonsBlocked[b] || s_State.ButtonsDown[b];
		}
		s_GamePointer = enabled;
	}

	void Input::WithholdFromGame(KeyCode key)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = KeyIndex(key);
		if (index < MaxKeys && s_State.KeysDown[index])
			s_State.GameKeysBlocked[index] = true;
	}

	void Input::WithholdFromGame(MouseCode button)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = ButtonIndex(button);
		if (index < MouseButtonCount && s_State.ButtonsDown[index])
			s_State.GameButtonsBlocked[index] = true;
	}

	bool Input::HasGamePointer()
	{
		std::scoped_lock lock(s_Mutex);
		return s_GamePointer;
	}

	void Input::SetCursorMode(CursorMode mode)
	{
		BS_CORE_ASSERT(static_cast<uint32_t>(mode) < CursorModeCount, "invalid cursor mode {}", static_cast<uint32_t>(mode));
		std::scoped_lock lock(s_Mutex);
		s_CursorMode = mode;
	}

	CursorMode Input::GetCursorMode()
	{
		std::scoped_lock lock(s_Mutex);
		return s_CursorMode;
	}

	void Input::AddTextCodepoint(uint32_t codepoint)
	{
		if (codepoint < 0x20U || codepoint == 0x7fU || codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU))
			return;
		std::string encoded;
		if (codepoint <= 0x7fU)
			encoded += static_cast<char>(codepoint);
		else
		{
			const int extra = codepoint <= 0x7ffU ? 1 : codepoint <= 0xffffU ? 2
																			 : 3;
			encoded += static_cast<char>((extra == 1 ? 0xc0U : extra == 2 ? 0xe0U
																		  : 0xf0U) |
										 (codepoint >> (6 * extra)));
			for (int shift = extra - 1; shift >= 0; shift--)
				encoded += static_cast<char>(0x80U | ((codepoint >> (6 * shift)) & 0x3fU));
		}
		std::scoped_lock lock(s_Mutex);
		if (s_TextInput.size() + encoded.size() > 16384)
			return;
		s_TextInput += encoded;
		if (s_GameFocus)
			s_GameTextInput += encoded;
	}

	std::string Input::GetTextInput()
	{
		std::scoped_lock lock(s_Mutex);
		return s_TextInput;
	}

	std::string GameInput::GetTextInput()
	{
		std::scoped_lock lock(s_Mutex);
		return s_GameFocus ? s_GameTextInput : std::string();
	}

	void Input::OnEvent(Event& event)
	{
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<KeyPressedEvent>([](KeyPressedEvent& e) { SetKeyState(e.GetKeyCode(), true); return false; });
		dispatcher.Dispatch<KeyTypedEvent>([](KeyTypedEvent& e) { AddTextCodepoint(e.GetCodepoint()); return false; });
		dispatcher.Dispatch<KeyReleasedEvent>([](KeyReleasedEvent& e) { SetKeyState(e.GetKeyCode(), false); return false; });
		dispatcher.Dispatch<MouseButtonPressedEvent>([](MouseButtonPressedEvent& e) { SetMouseButtonState(e.GetMouseButton(), true); return false; });
		dispatcher.Dispatch<MouseButtonReleasedEvent>([](MouseButtonReleasedEvent& e) { SetMouseButtonState(e.GetMouseButton(), false); return false; });
		dispatcher.Dispatch<MouseMovedEvent>([](MouseMovedEvent& e) { SetMousePosition({ e.GetX(), e.GetY() }); return false; });
		dispatcher.Dispatch<MouseScrolledEvent>([](MouseScrolledEvent& e) { AddMouseScroll({ e.GetXOffset(), e.GetYOffset() }); return false; });
		dispatcher.Dispatch<WindowLostFocusEvent>([](WindowLostFocusEvent&) { Reset(); return false; });
	}

	void Input::EndFrame()
	{
		std::scoped_lock lock(s_Mutex);
		// The game's view first: it reads the state this frame ends with. Blocks end when their control is let go
		// (SetKeyState, SetMouseButtonState, and every gamepad update).
		for (size_t k = 0; k < MaxKeys; k++)
			s_State.GameKeysLastFrame[k] = GameKeyDown(k);
		for (size_t b = 0; b < MouseButtonCount; b++)
			s_State.GameButtonsLastFrame[b] = GameButtonDown(b);
		for (GamepadSlot& gamepad : s_State.Gamepads)
		{
			for (size_t b = 0; b < GamepadButtonCount; b++)
				gamepad.GameButtonsLastFrame[b] = GameGamepadButtonDown(gamepad, b);
			for (size_t a = 0; a < GamepadAxisCount; a++)
				gamepad.GameAxesLastFrame[a] = GameGamepadAxis(gamepad, a);
			gamepad.ButtonsLastFrame = gamepad.Buttons;
			gamepad.AxesLastFrame = gamepad.Axes;
		}
		s_State.KeysDownLastFrame = s_State.KeysDown;
		s_State.ButtonsDownLastFrame = s_State.ButtonsDown;
		s_State.MousePositionLastFrame = s_State.MousePosition;
		s_State.Scroll = { 0.0f, 0.0f };
		s_TextInput.clear();
		s_GameTextInput.clear();
	}

	void Input::Reset()
	{
		std::scoped_lock lock(s_Mutex);
		s_State.KeysDown = {};
		s_State.KeysDownLastFrame = {};
		s_State.ButtonsDown = {};
		s_State.ButtonsDownLastFrame = {};
		s_State.MousePositionLastFrame = s_State.MousePosition;
		s_State.Scroll = { 0.0f, 0.0f };
		s_TextInput.clear();
		s_GameTextInput.clear();
		s_State.GameKeysBlocked = {};
		s_State.GameKeysLastFrame = {};
		s_State.GameButtonsBlocked = {};
		s_State.GameButtonsLastFrame = {};
		// Gamepads stay connected (a reset is not a disconnect: scripts would see a disconnect and a reconnect),
		// but release their buttons and center their sticks without reporting releases. A physical gamepad keeps
		// what was held released until it is let go: the next poll would otherwise press it again at once.
		for (GamepadSlot& gamepad : s_State.Gamepads)
		{
			if (gamepad.Source == GamepadSource::Device)
				gamepad.BlockHeld();
			gamepad.Buttons = {};
			gamepad.Axes = {};
			gamepad.ButtonsLastFrame = {};
			gamepad.AxesLastFrame = {};
			gamepad.ClearGameView();
		}
	}

	void Input::SetKeyState(KeyCode key, bool down)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = KeyIndex(key);
		if (index >= MaxKeys)
			return;
		s_State.KeysDown[index] = down;
		if (!down)
			s_State.GameKeysBlocked[index] = false;
	}

	void Input::SetMouseButtonState(MouseCode button, bool down)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = ButtonIndex(button);
		if (index >= MouseButtonCount)
			return;
		s_State.ButtonsDown[index] = down;
		if (!down)
			s_State.GameButtonsBlocked[index] = false;
	}

	void Input::SetMousePosition(const glm::vec2& position)
	{
		std::scoped_lock lock(s_Mutex);
		// The first sample must not produce a huge delta from the origin.
		if (!s_State.HasMousePosition)
		{
			s_State.MousePositionLastFrame = position;
			s_State.HasMousePosition = true;
		}
		s_State.MousePosition = position;
	}

	void Input::AddMouseScroll(const glm::vec2& scroll)
	{
		std::scoped_lock lock(s_Mutex);
		s_State.Scroll += scroll;
	}

	void Input::SetGamepadConnected(uint32_t index, bool connected, const std::string& name)
	{
		// Callers validate indices from data (the automation command; Lua has no setter).
		BS_CORE_ASSERT(index < MaxGamepads, "gamepad index {} out of range", index);
		std::scoped_lock lock(s_Mutex);
		if (index >= MaxGamepads)
			return;
		GamepadSlot& gamepad = s_State.Gamepads[index];
		if (!connected)
		{
			gamepad.Disconnect();
			return;
		}
		// A new or taken-over gamepad is a new connection and starts released; an injected one keeps its state when
		// renamed.
		if (gamepad.Source != GamepadSource::Injected)
		{
			gamepad.ClearInput();
			gamepad.Connection = NewConnection();
		}
		gamepad.Source = GamepadSource::Injected;
		gamepad.Name = name;
	}

	bool Input::SetGamepadButtonState(uint32_t index, GamepadButton button, bool down)
	{
		std::scoped_lock lock(s_Mutex);
		GamepadSlot* gamepad = ConnectedGamepad(index);
		const auto b = static_cast<size_t>(button);
		if (!gamepad || gamepad->Source != GamepadSource::Injected || b >= GamepadButtonCount)
			return false;
		gamepad->Buttons[b] = down;
		gamepad->ReleaseGameBlocks();
		return true;
	}

	bool Input::SetGamepadAxis(uint32_t index, GamepadAxis axis, float value)
	{
		std::scoped_lock lock(s_Mutex);
		GamepadSlot* gamepad = ConnectedGamepad(index);
		const auto a = static_cast<size_t>(axis);
		if (!gamepad || gamepad->Source != GamepadSource::Injected || a >= GamepadAxisCount)
			return false;
		gamepad->Axes[a] = ClampAxis(axis, value);
		gamepad->ReleaseGameBlocks();
		return true;
	}

	void Input::ForgetInjectedGamepads()
	{
		std::scoped_lock lock(s_Mutex);
		ForgetGamepads(GamepadSource::Injected);
	}

	void Input::SetDeviceGamepadsEnabled(bool enabled)
	{
		std::scoped_lock lock(s_Mutex);
		s_DeviceGamepadsEnabled = enabled;
		if (!enabled)
			ForgetGamepads(GamepadSource::Device);
	}

	bool Input::IsDeviceGamepadsEnabled()
	{
		std::scoped_lock lock(s_Mutex);
		return s_DeviceGamepadsEnabled;
	}

	void Input::UpdateGamepadFromDevice(uint32_t index, const GamepadDeviceState* state, bool focused)
	{
		std::scoped_lock lock(s_Mutex);
		if (index >= MaxGamepads || !s_DeviceGamepadsEnabled || s_State.Gamepads[index].Source == GamepadSource::Injected)
			return;
		GamepadSlot& gamepad = s_State.Gamepads[index];
		if (!state)
		{
			gamepad.Disconnect();
			return;
		}
		if (gamepad.Source == GamepadSource::None)
			gamepad.Connection = NewConnection();
		gamepad.Source = GamepadSource::Device;
		gamepad.Name = state->Name;
		// Unfocused, everything held is blocked (and reads as released); only a real release unblocks it.
		for (size_t b = 0; b < GamepadButtonCount; b++)
		{
			const bool held = state->Buttons[b];
			gamepad.ButtonBlocked[b] = held && (gamepad.ButtonBlocked[b] || !focused);
			gamepad.Buttons[b] = held && !gamepad.ButtonBlocked[b];
		}
		for (size_t a = 0; a < GamepadAxisCount; a++)
		{
			const float value = ClampAxis(static_cast<GamepadAxis>(a), state->Axes[a]);
			gamepad.AxisBlocked[a] = IsAxisPushed(value) && (gamepad.AxisBlocked[a] || !focused);
			gamepad.Axes[a] = focused && !gamepad.AxisBlocked[a] ? value : 0.0f;
		}
		gamepad.ReleaseGameBlocks();
	}

	bool GameInput::IsKeyDown(KeyCode key, InputFrame frame)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = KeyIndex(key);
		if (index >= MaxKeys)
			return false;
		return frame == InputFrame::Current ? GameKeyDown(index) : s_State.GameKeysLastFrame[index];
	}

	bool GameInput::IsKeyPressed(KeyCode key)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = KeyIndex(key);
		return index < MaxKeys && GameKeyDown(index) && !s_State.GameKeysLastFrame[index];
	}

	bool GameInput::IsKeyReleased(KeyCode key)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = KeyIndex(key);
		return index < MaxKeys && !GameKeyDown(index) && s_State.GameKeysLastFrame[index];
	}

	bool GameInput::IsMouseButtonDown(MouseCode button, InputFrame frame)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = ButtonIndex(button);
		if (index >= MouseButtonCount)
			return false;
		return frame == InputFrame::Current ? GameButtonDown(index) : s_State.GameButtonsLastFrame[index];
	}

	bool GameInput::IsMouseButtonPressed(MouseCode button)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = ButtonIndex(button);
		return index < MouseButtonCount && GameButtonDown(index) && !s_State.GameButtonsLastFrame[index];
	}

	bool GameInput::IsMouseButtonReleased(MouseCode button)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = ButtonIndex(button);
		return index < MouseButtonCount && !GameButtonDown(index) && s_State.GameButtonsLastFrame[index];
	}

	glm::vec2 GameInput::GetMouseDelta()
	{
		std::scoped_lock lock(s_Mutex);
		return s_GamePointer ? s_State.MousePosition - s_State.MousePositionLastFrame : glm::vec2(0.0f);
	}

	glm::vec2 GameInput::GetMouseScroll()
	{
		std::scoped_lock lock(s_Mutex);
		return s_GamePointer ? s_State.Scroll : glm::vec2(0.0f);
	}

	bool GameInput::IsGamepadButtonDown(uint32_t index, GamepadButton button, InputFrame frame)
	{
		std::scoped_lock lock(s_Mutex);
		const auto b = static_cast<size_t>(button);
		if (b >= GamepadButtonCount)
			return false;
		if (frame == InputFrame::Previous)
			return index < Input::MaxGamepads && s_State.Gamepads[index].GameButtonsLastFrame[b];
		const GamepadSlot* gamepad = ConnectedGamepad(index);
		return gamepad && GameGamepadButtonDown(*gamepad, b);
	}

	bool GameInput::IsGamepadButtonPressed(uint32_t index, GamepadButton button)
	{
		std::scoped_lock lock(s_Mutex);
		const GamepadSlot* gamepad = ConnectedGamepad(index);
		const auto b = static_cast<size_t>(button);
		return gamepad && b < GamepadButtonCount && GameGamepadButtonDown(*gamepad, b) && !gamepad->GameButtonsLastFrame[b];
	}

	bool GameInput::IsGamepadButtonReleased(uint32_t index, GamepadButton button)
	{
		std::scoped_lock lock(s_Mutex);
		// Like Input's: a gamepad disconnected while a button was held reports the release on that frame.
		const auto b = static_cast<size_t>(button);
		if (index >= Input::MaxGamepads || b >= GamepadButtonCount)
			return false;
		const GamepadSlot& gamepad = s_State.Gamepads[index];
		return !(gamepad.IsConnected() && GameGamepadButtonDown(gamepad, b)) && gamepad.GameButtonsLastFrame[b];
	}

	float GameInput::GetGamepadAxis(uint32_t index, GamepadAxis axis, InputFrame frame)
	{
		std::scoped_lock lock(s_Mutex);
		const auto a = static_cast<size_t>(axis);
		if (a >= GamepadAxisCount)
			return 0.0f;
		if (frame == InputFrame::Previous)
			return index < Input::MaxGamepads ? s_State.Gamepads[index].GameAxesLastFrame[a] : 0.0f;
		const GamepadSlot* gamepad = ConnectedGamepad(index);
		return gamepad ? GameGamepadAxis(*gamepad, a) : 0.0f;
	}

}
