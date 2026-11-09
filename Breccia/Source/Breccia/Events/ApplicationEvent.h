#pragma once

#include "Breccia/Events/Event.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Breccia {

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

		BR_EVENT_CLASS_TYPE(WindowResize)
		BR_EVENT_CLASS_CATEGORY(EventCategoryApplication)

	private:
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
	};

	class WindowCloseEvent : public Event
	{
	public:
		BR_EVENT_CLASS_TYPE(WindowClose)
		BR_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class WindowFocusEvent : public Event
	{
	public:
		BR_EVENT_CLASS_TYPE(WindowFocus)
		BR_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class WindowLostFocusEvent : public Event
	{
	public:
		BR_EVENT_CLASS_TYPE(WindowLostFocus)
		BR_EVENT_CLASS_CATEGORY(EventCategoryApplication)
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

		BR_EVENT_CLASS_TYPE(WindowDrop)
		BR_EVENT_CLASS_CATEGORY(EventCategoryApplication)

	private:
		std::vector<std::filesystem::path> m_Paths;
	};

}
