#include "Basalt/Core/Input.h"

#include "Basalt/Events/ApplicationEvent.h"
#include "Basalt/Events/KeyEvent.h"
#include "Basalt/Events/MouseEvent.h"

#include <array>
#include <mutex>

namespace Basalt {

	namespace {

		constexpr size_t MaxKeys = 512;

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

	void Input::OnEvent(Event& event)
	{
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<KeyPressedEvent>([](KeyPressedEvent& e)
											 { SetKeyState(e.GetKeyCode(), true); return false; });
		dispatcher.Dispatch<KeyReleasedEvent>([](KeyReleasedEvent& e)
											  { SetKeyState(e.GetKeyCode(), false); return false; });
		dispatcher.Dispatch<MouseButtonPressedEvent>([](MouseButtonPressedEvent& e)
													 { SetMouseButtonState(e.GetMouseButton(), true); return false; });
		dispatcher.Dispatch<MouseButtonReleasedEvent>([](MouseButtonReleasedEvent& e)
													  { SetMouseButtonState(e.GetMouseButton(), false); return false; });
		dispatcher.Dispatch<MouseMovedEvent>([](MouseMovedEvent& e)
											 { SetMousePosition({ e.GetX(), e.GetY() }); return false; });
		dispatcher.Dispatch<MouseScrolledEvent>([](MouseScrolledEvent& e)
												{ AddMouseScroll({ e.GetXOffset(), e.GetYOffset() }); return false; });
		dispatcher.Dispatch<WindowLostFocusEvent>([](WindowLostFocusEvent&)
												  { Reset(); return false; });
	}

	void Input::EndFrame()
	{
		std::scoped_lock lock(s_Mutex);
		s_State.KeysDownLastFrame = s_State.KeysDown;
		s_State.ButtonsDownLastFrame = s_State.ButtonsDown;
		s_State.MousePositionLastFrame = s_State.MousePosition;
		s_State.Scroll = { 0.0f, 0.0f };
	}

	void Input::Reset()
	{
		std::scoped_lock lock(s_Mutex);
		const glm::vec2 position = s_State.MousePosition;
		const bool hasPosition = s_State.HasMousePosition;
		s_State = {};
		s_State.MousePosition = position;
		s_State.MousePositionLastFrame = position;
		s_State.HasMousePosition = hasPosition;
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

}
