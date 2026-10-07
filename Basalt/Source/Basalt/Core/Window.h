#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Events/Event.h"

#include <functional>
#include <string>

struct GLFWwindow;

namespace Basalt {

	struct WindowSpecification
	{
		std::string Title = "Basalt";
		uint32_t Width = 1600;
		uint32_t Height = 900;
		bool Resizable = true;
		bool StartMaximized = false;
		bool Visible = true;
	};

	// GLFW window without a client API; the swapchain is owned by the GraphicsDevice.
	class Window
	{
	public:
		using EventCallbackFn = std::function<void(Event&)>;

		explicit Window(const WindowSpecification& specification);
		~Window();

		Window(const Window&) = delete;
		Window& operator=(const Window&) = delete;

		void PollEvents();

		uint32_t GetWidth() const { return m_Data.Width; }
		uint32_t GetHeight() const { return m_Data.Height; }
		// Size in pixels (differs from the window size on high-DPI displays).
		void GetFramebufferSize(uint32_t& outWidth, uint32_t& outHeight) const;
		float GetContentScale() const;
		bool IsMinimized() const;
		// False when the native window could not be created.
		bool IsValid() const { return m_Window != nullptr; }

		void SetTitle(const std::string& title);
		const std::string& GetTitle() const { return m_Data.Title; }
		void SetEventCallback(const EventCallbackFn& callback) { m_Data.EventCallback = callback; }
		void SetCursorLocked(bool locked);

		GLFWwindow* GetNativeWindow() const { return m_Window; }

	private:
		struct WindowData
		{
			std::string Title;
			uint32_t Width = 0;
			uint32_t Height = 0;
			EventCallbackFn EventCallback;
		};

		GLFWwindow* m_Window = nullptr;
		WindowData m_Data;
	};

}
