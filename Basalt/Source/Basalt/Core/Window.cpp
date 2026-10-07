#include "Basalt/Core/Window.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Events/ApplicationEvent.h"
#include "Basalt/Events/KeyEvent.h"
#include "Basalt/Events/MouseEvent.h"

#include <GLFW/glfw3.h>

namespace Basalt {

	namespace {

		uint32_t s_WindowCount = 0;

		void GLFWErrorCallback(int error, const char* description)
		{
			BS_CORE_ERROR("GLFW error ({}): {}", error, description);
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

	void Window::SetCursorLocked(bool locked)
	{
		glfwSetInputMode(m_Window, GLFW_CURSOR, locked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
	}

}
