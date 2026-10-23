#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Core/Input.h"
#include "Basalt/Events/Event.h"

#include <functional>
#include <span>
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

	// One size of a window icon: RGBA8 pixels, top row first, owned by the caller.
	struct WindowIcon
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		const uint8_t* Pixels = nullptr;
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
		// False where windows have no icon of their own: macOS shows the app bundle's icon and Wayland the desktop
		// entry's (GLFW reports an error there).
		bool SupportsIcon() const;
		// Several sizes of one icon; the window system picks the closest to each use. Ignored where !SupportsIcon().
		void SetIcon(std::span<const WindowIcon> images);
		const std::string& GetTitle() const { return m_Data.Title; }
		void SetEventCallback(const EventCallbackFn& callback) { m_Data.EventCallback = callback; }
		// Locked uses raw (unaccelerated) mouse motion where the platform has it, as first-person cameras want.
		void SetCursorMode(CursorMode mode);

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
