#include "Basalt/Core/Input.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Events/ApplicationEvent.h"
#include "Basalt/Events/KeyEvent.h"
#include "Basalt/Events/MouseEvent.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace Basalt {

	namespace {

		constexpr size_t MaxKeys = 512;

		// A stick or trigger counts as let go below this (see GamepadSlot::ButtonBlocked).
		constexpr float AxisRestThreshold = 0.25f;

		struct GamepadSlot
		{
			GamepadSource Source = GamepadSource::None;
			// Input::GetGamepadConnection: new for every connect, 0 while disconnected.
			uint32_t Connection = 0;
			std::string Name;
			std::array<bool, GamepadButtonCount> Buttons = {};
			std::array<bool, GamepadButtonCount> ButtonsLastFrame = {};
			std::array<float, GamepadAxisCount> Axes = {};
			// A physical button or axis held through a reset or while the window was unfocused: it reads as released
			// until the device lets it go, so it does not press again by itself.
			std::array<bool, GamepadButtonCount> ButtonBlocked = {};
			std::array<bool, GamepadAxisCount> AxisBlocked = {};

			bool IsConnected() const { return Source != GamepadSource::None; }

			void ClearInput()
			{
				Buttons = {};
				Axes = {};
				ButtonBlocked = {};
				AxisBlocked = {};
			}

			// Blocks what is held now (see ButtonBlocked).
			void BlockHeld()
			{
				for (size_t b = 0; b < GamepadButtonCount; b++)
					ButtonBlocked[b] = ButtonBlocked[b] || Buttons[b];
				for (size_t a = 0; a < GamepadAxisCount; a++)
					AxisBlocked[a] = AxisBlocked[a] || std::abs(Axes[a]) >= AxisRestThreshold;
			}

			// Keeps ButtonsLastFrame, so the buttons held reports their release on this frame.
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
		};

		// All guarded by s_Mutex. The settings outlive Reset, which only clears what is held.
		std::mutex s_Mutex;
		InputState s_State;
		bool s_DeviceGamepadsEnabled = true;
		uint32_t s_NextGamepadConnection = 1;

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

	}

	bool Input::IsKeyDown(KeyCode key)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = KeyIndex(key);
		return index < MaxKeys && s_State.KeysDown[index];
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

	bool Input::IsMouseButtonDown(MouseCode button)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = ButtonIndex(button);
		return index < MouseButtonCount && s_State.ButtonsDown[index];
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

	bool Input::IsGamepadButtonDown(uint32_t index, GamepadButton button)
	{
		std::scoped_lock lock(s_Mutex);
		const GamepadSlot* gamepad = ConnectedGamepad(index);
		const auto b = static_cast<size_t>(button);
		return gamepad && b < GamepadButtonCount && gamepad->Buttons[b];
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

	float Input::GetGamepadAxis(uint32_t index, GamepadAxis axis)
	{
		std::scoped_lock lock(s_Mutex);
		const GamepadSlot* gamepad = ConnectedGamepad(index);
		const auto a = static_cast<size_t>(axis);
		return gamepad && a < GamepadAxisCount ? gamepad->Axes[a] : 0.0f;
	}

	void Input::OnEvent(Event& event)
	{
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<KeyPressedEvent>([](KeyPressedEvent& e) { SetKeyState(e.GetKeyCode(), true); return false; });
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
		s_State.KeysDownLastFrame = s_State.KeysDown;
		s_State.ButtonsDownLastFrame = s_State.ButtonsDown;
		s_State.MousePositionLastFrame = s_State.MousePosition;
		s_State.Scroll = { 0.0f, 0.0f };
		for (GamepadSlot& gamepad : s_State.Gamepads)
			gamepad.ButtonsLastFrame = gamepad.Buttons;
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
		}
	}

	void Input::SetKeyState(KeyCode key, bool down)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = KeyIndex(key);
		if (index < MaxKeys)
			s_State.KeysDown[index] = down;
	}

	void Input::SetMouseButtonState(MouseCode button, bool down)
	{
		std::scoped_lock lock(s_Mutex);
		const size_t index = ButtonIndex(button);
		if (index < MouseButtonCount)
			s_State.ButtonsDown[index] = down;
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
			const bool pushed = std::abs(value) >= AxisRestThreshold;
			gamepad.AxisBlocked[a] = pushed && (gamepad.AxisBlocked[a] || !focused);
			gamepad.Axes[a] = focused && !gamepad.AxisBlocked[a] ? value : 0.0f;
		}
	}

}
