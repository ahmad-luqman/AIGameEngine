#pragma once

#include "Basalt/Events/Event.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Basalt {

	class WindowResizeEvent : public Event
	{
	public:
		WindowResizeEvent(uint32_t width, uint32_t height)
			: m_Width(width)
			, m_Height(height)
		{
		}

		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }

		std::string ToString() const override
		{
			return "WindowResizeEvent: " + std::to_string(m_Width) + ", " + std::to_string(m_Height);
		}

		BS_EVENT_CLASS_TYPE(WindowResize)
		BS_EVENT_CLASS_CATEGORY(EventCategoryApplication)

	private:
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
	};

	class WindowCloseEvent : public Event
	{
	public:
		BS_EVENT_CLASS_TYPE(WindowClose)
		BS_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class WindowFocusEvent : public Event
	{
	public:
		BS_EVENT_CLASS_TYPE(WindowFocus)
		BS_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class WindowLostFocusEvent : public Event
	{
	public:
		BS_EVENT_CLASS_TYPE(WindowLostFocus)
		BS_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	// Files dragged onto the window.
	class WindowDropEvent : public Event
	{
	public:
		explicit WindowDropEvent(std::vector<std::filesystem::path> paths)
			: m_Paths(std::move(paths))
		{
		}

		const std::vector<std::filesystem::path>& GetPaths() const { return m_Paths; }

		BS_EVENT_CLASS_TYPE(WindowDrop)
		BS_EVENT_CLASS_CATEGORY(EventCategoryApplication)

	private:
		std::vector<std::filesystem::path> m_Paths;
	};

}
