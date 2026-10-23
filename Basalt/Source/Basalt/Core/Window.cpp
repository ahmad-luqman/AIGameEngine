#include "Basalt/Core/Window.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Input.h"
#include "Basalt/Core/Platform.h"
#include "Basalt/Events/ApplicationEvent.h"
#include "Basalt/Events/KeyEvent.h"
#include "Basalt/Events/MouseEvent.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Basalt {

	namespace {

		uint32_t s_WindowCount = 0;

		void GLFWErrorCallback(int error, const char* description)
		{
			BS_CORE_ERROR("GLFW error ({}): {}", error, description);
		}

		// Adds the gamepad mappings of the SDL_GAMECONTROLLERCONFIG environment variable (SDL's format, one mapping per
		// line) to GLFW's built-in database, for controllers it does not know.
		void LoadGamepadMappings()
		{
			const std::optional<std::string> mappings = Platform::GetEnvVar("SDL_GAMECONTROLLERCONFIG");
			if (!mappings || mappings->empty())
				return;
			if (glfwUpdateGamepadMappings(mappings->c_str()))
				BS_CORE_INFO("Input: added the gamepad mappings of SDL_GAMECONTROLLERCONFIG");
			else
				BS_CORE_ERROR("Input: SDL_GAMECONTROLLERCONFIG has invalid gamepad mappings (see the GLFW error above); they are ignored");
		}

		// Feeds Input with every joystick GLFW maps to the standard gamepad layout (its SDL mapping database plus
		// SDL_GAMECONTROLLERCONFIG). Joysticks without a mapping are not gamepads and read as disconnected, with a
		// warning once per controller model. While the window does not have focus, gamepads stay connected but read
		// as released (Input::UpdateGamepadFromDevice): like the keyboard, they do not drive an unfocused game.
		void PollGamepads(bool focused)
		{
			static_assert(static_cast<uint32_t>(GLFW_GAMEPAD_BUTTON_LAST + 1) == GamepadButtonCount && static_cast<uint32_t>(GLFW_GAMEPAD_AXIS_LAST + 1) == GamepadAxisCount);
			static_assert(GLFW_GAMEPAD_BUTTON_A == static_cast<int>(GamepadButton::A) && GLFW_GAMEPAD_BUTTON_DPAD_LEFT == static_cast<int>(GamepadButton::DPadLeft));
			static_assert(GLFW_GAMEPAD_AXIS_LEFT_TRIGGER == static_cast<int>(GamepadAxis::LeftTrigger));
			static std::unordered_set<std::string> s_ReportedUnmapped;
			static std::unordered_set<int> s_ReportedUnreadable;
			constexpr int Count = std::min(static_cast<int>(Input::MaxGamepads), GLFW_JOYSTICK_LAST + 1);
			for (int id = 0; id < Count; id++)
			{
				const auto index = static_cast<uint32_t>(id);
				if (!glfwJoystickIsGamepad(id))
				{
					if (glfwJoystickPresent(id))
					{
						const char* guid = glfwGetJoystickGUID(id);
						const char* name = glfwGetJoystickName(id);
						if (s_ReportedUnmapped.insert(fmt::format("{}/{}", guid ? guid : "", name ? name : "")).second)
							BS_CORE_WARN("Input: joystick '{}' (GUID {}) has no gamepad mapping and is ignored; add a mapping for it to the SDL_GAMECONTROLLERCONFIG environment variable", name ? name : "?", guid ? guid : "?");
					}
					Input::UpdateGamepadFromDevice(index, nullptr);
					continue;
				}
				GamepadDeviceState device;
				const char* name = glfwGetGamepadName(id);
				device.Name = name ? name : "Gamepad";
				GLFWgamepadstate state;
				// Unplugged between the two calls (the next poll sees it gone) or a platform error: released.
				if (!glfwGetGamepadState(id, &state))
				{
					if (s_ReportedUnreadable.insert(id).second)
						BS_CORE_WARN("Input: could not read gamepad {} ('{}'); it reads as released", id, device.Name);
					Input::UpdateGamepadFromDevice(index, &device, focused);
					continue;
				}
				for (uint32_t b = 0; b < GamepadButtonCount; b++)
					device.Buttons[b] = state.buttons[b] == GLFW_PRESS;
				for (uint32_t a = 0; a < GamepadAxisCount; a++)
				{
					// GLFW reports triggers from -1 (released) to 1, like the sticks.
					const float value = state.axes[a];
					device.Axes[a] = IsGamepadTrigger(static_cast<GamepadAxis>(a)) ? (value + 1.0f) * 0.5f : value;
				}
				Input::UpdateGamepadFromDevice(index, &device, focused);
			}
		}
	}

	Window::Window(const WindowSpecification& specification)
	{
		m_Data.Title = specification.Title;
		m_Data.Width = specification.Width;
		m_Data.Height = specification.Height;

		if (s_WindowCount == 0)
		{
			glfwSetErrorCallback(GLFWErrorCallback);
			if (!glfwInit())
			{
				BS_CORE_ERROR("Could not initialize GLFW (no display available?)");
				return;
			}
			LoadGamepadMappings();
		}

		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_RESIZABLE, specification.Resizable ? GLFW_TRUE : GLFW_FALSE);
		glfwWindowHint(GLFW_MAXIMIZED, specification.StartMaximized ? GLFW_TRUE : GLFW_FALSE);
		glfwWindowHint(GLFW_VISIBLE, specification.Visible ? GLFW_TRUE : GLFW_FALSE);

		m_Window = glfwCreateWindow(static_cast<int>(m_Data.Width), static_cast<int>(m_Data.Height), m_Data.Title.c_str(), nullptr, nullptr);
		if (!m_Window)
		{
			BS_CORE_ERROR("Could not create a window");
			if (s_WindowCount == 0)
				glfwTerminate();
			return;
		}
		s_WindowCount++;

		int width = 0;
		int height = 0;
		glfwGetWindowSize(m_Window, &width, &height);
		m_Data.Width = static_cast<uint32_t>(width);
		m_Data.Height = static_cast<uint32_t>(height);

		glfwSetWindowUserPointer(m_Window, &m_Data);

		glfwSetWindowSizeCallback(m_Window, [](GLFWwindow* window, int newWidth, int newHeight) {
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			data.Width = static_cast<uint32_t>(newWidth);
			data.Height = static_cast<uint32_t>(newHeight);
			WindowResizeEvent event(data.Width, data.Height);
			if (data.EventCallback)
				data.EventCallback(event); });

		glfwSetWindowCloseCallback(m_Window, [](GLFWwindow* window) {
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			WindowCloseEvent event;
			if (data.EventCallback)
				data.EventCallback(event); });

		glfwSetWindowFocusCallback(m_Window, [](GLFWwindow* window, int focused) {
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			if (!data.EventCallback)
				return;
			if (focused)
			{
				WindowFocusEvent event;
				data.EventCallback(event);
			}
			else
			{
				WindowLostFocusEvent event;
				data.EventCallback(event);
			} });

		glfwSetKeyCallback(m_Window, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			if (!data.EventCallback || key < 0)
				return;
			const auto keyCode = static_cast<KeyCode>(key);
			switch (action)
			{
				case GLFW_PRESS:
				{
					KeyPressedEvent event(keyCode, false);
					data.EventCallback(event);
					break;
				}
				case GLFW_RELEASE:
				{
					KeyReleasedEvent event(keyCode);
					data.EventCallback(event);
					break;
				}
				case GLFW_REPEAT:
				{
					KeyPressedEvent event(keyCode, true);
					data.EventCallback(event);
					break;
				}
				default:
					break;
			} });

		glfwSetCharCallback(m_Window, [](GLFWwindow* window, unsigned int codepoint) {
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			KeyTypedEvent event(codepoint);
			if (data.EventCallback)
				data.EventCallback(event); });

		glfwSetMouseButtonCallback(m_Window, [](GLFWwindow* window, int button, int action, int mods) {
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			if (!data.EventCallback || button < 0 || button >= static_cast<int>(MouseButtonCount))
				return;
			const auto mouseCode = static_cast<MouseCode>(button);
			if (action == GLFW_PRESS)
			{
				MouseButtonPressedEvent event(mouseCode);
				data.EventCallback(event);
			}
			else if (action == GLFW_RELEASE)
			{
				MouseButtonReleasedEvent event(mouseCode);
				data.EventCallback(event);
			} });

		glfwSetScrollCallback(m_Window, [](GLFWwindow* window, double xOffset, double yOffset) {
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			MouseScrolledEvent event(static_cast<float>(xOffset), static_cast<float>(yOffset));
			if (data.EventCallback)
				data.EventCallback(event); });

		glfwSetCursorPosCallback(m_Window, [](GLFWwindow* window, double x, double y) {
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			MouseMovedEvent event(static_cast<float>(x), static_cast<float>(y));
			if (data.EventCallback)
				data.EventCallback(event); });

		glfwSetDropCallback(m_Window, [](GLFWwindow* window, int count, const char** paths) {
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			std::vector<std::filesystem::path> droppedPaths;
			droppedPaths.reserve(static_cast<size_t>(count));
			for (int i = 0; i < count; i++)
				droppedPaths.emplace_back(paths[i]);
			WindowDropEvent event(std::move(droppedPaths));
			if (data.EventCallback)
				data.EventCallback(event); });
	}

	Window::~Window()
	{
		if (!m_Window)
			return;

		glfwDestroyWindow(m_Window);
		m_Window = nullptr;
		if (--s_WindowCount == 0)
			glfwTerminate();
	}

	void Window::PollEvents()
	{
		glfwPollEvents();
		PollGamepads(glfwGetWindowAttrib(m_Window, GLFW_FOCUSED) == GLFW_TRUE);
	}

	void Window::GetFramebufferSize(uint32_t& outWidth, uint32_t& outHeight) const
	{
		int width = 0;
		int height = 0;
		glfwGetFramebufferSize(m_Window, &width, &height);
		outWidth = static_cast<uint32_t>(width);
		outHeight = static_cast<uint32_t>(height);
	}

	float Window::GetContentScale() const
	{
		float xScale = 1.0f;
		float yScale = 1.0f;
		glfwGetWindowContentScale(m_Window, &xScale, &yScale);
		return xScale;
	}

	bool Window::IsMinimized() const
	{
		uint32_t width = 0;
		uint32_t height = 0;
		GetFramebufferSize(width, height);
		return width == 0 || height == 0 || glfwGetWindowAttrib(m_Window, GLFW_ICONIFIED);
	}

	void Window::SetTitle(const std::string& title)
	{
		m_Data.Title = title;
		glfwSetWindowTitle(m_Window, title.c_str());
	}

	bool Window::SupportsIcon() const
	{
		if (!m_Window)
			return false;
		const int platform = glfwGetPlatform();
		return platform == GLFW_PLATFORM_WIN32 || platform == GLFW_PLATFORM_X11;
	}

	void Window::SetIcon(std::span<const WindowIcon> images)
	{
		if (!SupportsIcon())
			return;
		std::vector<GLFWimage> glfwImages;
		glfwImages.reserve(images.size());
		for (const WindowIcon& image : images)
		{
			BS_CORE_ASSERT(image.Width > 0 && image.Height > 0 && image.Pixels.size() == static_cast<size_t>(image.Width) * image.Height * 4, "window icon pixels do not match its size");
			// GLFW copies the pixels and never writes them; its struct is simply not const-qualified.
			glfwImages.push_back({ static_cast<int>(image.Width), static_cast<int>(image.Height), const_cast<unsigned char*>(image.Pixels.data()) });
		}
		glfwSetWindowIcon(m_Window, static_cast<int>(glfwImages.size()), glfwImages.data());
	}

	void Window::SetCursorMode(CursorMode mode)
	{
		if (!m_Window)
			return;
		int glfwMode = GLFW_CURSOR_NORMAL;
		switch (mode)
		{
			case CursorMode::Normal:
				glfwMode = GLFW_CURSOR_NORMAL;
				break;
			case CursorMode::Hidden:
				glfwMode = GLFW_CURSOR_HIDDEN;
				break;
			case CursorMode::Locked:
				glfwMode = GLFW_CURSOR_DISABLED;
				break;
		}
		glfwSetInputMode(m_Window, GLFW_CURSOR, glfwMode);
		if (glfwRawMouseMotionSupported())
			glfwSetInputMode(m_Window, GLFW_RAW_MOUSE_MOTION, mode == CursorMode::Locked ? GLFW_TRUE : GLFW_FALSE);
	}

}
