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

		// A stick or trigger counts as let go below this (after a reset, see GamepadSlot::Blocked).
		constexpr float AxisRestThreshold = 0.25f;

		struct GamepadSlot
		{
			GamepadSource Source = GamepadSource::None;
			bool ConnectedLastFrame = false;
			std::string Name;
			std::array<bool, GamepadButtonCount> Buttons = {};
			std::array<bool, GamepadButtonCount> ButtonsLastFrame = {};
			std::array<float, GamepadAxisCount> Axes = {};
			// A physical button or axis held through a reset: it reads as released until the device lets it go.
			std::array<bool, GamepadButtonCount> ButtonBlocked = {};
			std::array<bool, GamepadAxisCount> AxisBlocked = {};

			bool IsConnected() const { return Source != GamepadSource::None; }

			void Release()
			{
				Buttons = {};
				Axes = {};
			}

			void Disconnect()
			{
				Release();
				Source = GamepadSource::None;
				Name.clear();
				ButtonBlocked = {};
				AxisBlocked = {};
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
			bool DeviceGamepadsEnabled = true;
		};

		std::mutex s_Mutex;
		InputState s_State;

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

	bool Input::WasGamepadConnected(uint32_t index)
	{
		std::scoped_lock lock(s_Mutex);
		const GamepadSlot* gamepad = ConnectedGamepad(index);
		return gamepad && !gamepad->ConnectedLastFrame;
	}

	bool Input::WasGamepadDisconnected(uint32_t index)
	{
		std::scoped_lock lock(s_Mutex);
		return index < MaxGamepads && !s_State.Gamepads[index].IsConnected() && s_State.Gamepads[index].ConnectedLastFrame;
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
		{
			gamepad.ConnectedLastFrame = gamepad.IsConnected();
			gamepad.ButtonsLastFrame = gamepad.Buttons;
		}
	}

	void Input::Reset()
	{
		std::scoped_lock lock(s_Mutex);
		const glm::vec2 position = s_State.MousePosition;
		const bool hasPosition = s_State.HasMousePosition;
		// Gamepads stay connected (a reset is not a disconnect: scripts would see a disconnect and a reconnect),
		// but release their buttons and center their sticks without reporting releases. A physical gamepad keeps
		// what was held released until it is let go: the next poll would otherwise press it again at once.
		const bool deviceGamepadsEnabled = s_State.DeviceGamepadsEnabled;
		std::array<GamepadSlot, MaxGamepads> gamepads = s_State.Gamepads;
		for (GamepadSlot& gamepad : gamepads)
		{
			if (gamepad.Source == GamepadSource::Device)
			{
				for (size_t b = 0; b < GamepadButtonCount; b++)
					gamepad.ButtonBlocked[b] = gamepad.ButtonBlocked[b] || gamepad.Buttons[b];
				for (size_t a = 0; a < GamepadAxisCount; a++)
					gamepad.AxisBlocked[a] = gamepad.AxisBlocked[a] || std::abs(gamepad.Axes[a]) >= AxisRestThreshold;
			}
			gamepad.Release();
			gamepad.ButtonsLastFrame = {};
		}
		s_State = {};
		s_State.MousePosition = position;
		s_State.MousePositionLastFrame = position;
		s_State.HasMousePosition = hasPosition;
		s_State.Gamepads = std::move(gamepads);
		s_State.DeviceGamepadsEnabled = deviceGamepadsEnabled;
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
		// Callers validate indices from data (the automation command, Lua has no setter).
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
		// A new or taken-over gamepad starts released; an injected one keeps its state when renamed.
		if (gamepad.Source != GamepadSource::Injected)
		{
			gamepad.Release();
			gamepad.ButtonBlocked = {};
			gamepad.AxisBlocked = {};
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
		for (GamepadSlot& gamepad : s_State.Gamepads)
		{
			if (gamepad.Source == GamepadSource::Injected)
				gamepad = {};
		}
	}

	void Input::SetDeviceGamepadsEnabled(bool enabled)
	{
		std::scoped_lock lock(s_Mutex);
		s_State.DeviceGamepadsEnabled = enabled;
		if (enabled)
			return;
		for (GamepadSlot& gamepad : s_State.Gamepads)
		{
			if (gamepad.Source == GamepadSource::Device)
				gamepad = {};
		}
	}

	void Input::UpdateGamepadFromDevice(uint32_t index, const GamepadDeviceState* state)
	{
		std::scoped_lock lock(s_Mutex);
		if (index >= MaxGamepads || !s_State.DeviceGamepadsEnabled || s_State.Gamepads[index].Source == GamepadSource::Injected)
			return;
		GamepadSlot& gamepad = s_State.Gamepads[index];
		if (!state)
		{
			gamepad.Disconnect();
			return;
		}
		gamepad.Source = GamepadSource::Device;
		gamepad.Name = state->Name;
		for (size_t b = 0; b < GamepadButtonCount; b++)
		{
			if (gamepad.ButtonBlocked[b] && !state->Buttons[b])
				gamepad.ButtonBlocked[b] = false;
			gamepad.Buttons[b] = state->Buttons[b] && !gamepad.ButtonBlocked[b];
		}
		for (size_t a = 0; a < GamepadAxisCount; a++)
		{
			const float value = ClampAxis(static_cast<GamepadAxis>(a), state->Axes[a]);
			if (gamepad.AxisBlocked[a] && std::abs(value) < AxisRestThreshold)
				gamepad.AxisBlocked[a] = false;
			gamepad.Axes[a] = gamepad.AxisBlocked[a] ? 0.0f : value;
		}
	}

}
