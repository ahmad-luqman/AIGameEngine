#pragma once

#include "Breccia/Core/MouseCodes.h"
#include "Breccia/Events/Event.h"

namespace Breccia {

	class MouseMovedEvent : public Event
	{
	public:
		MouseMovedEvent(float x, float y)
			: m_MouseX(x)
			, m_MouseY(y)
		{
		}

		float GetX() const { return m_MouseX; }
		float GetY() const { return m_MouseY; }

		BR_EVENT_CLASS_TYPE(MouseMoved)
		BR_EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)

	private:
		float m_MouseX = 0.0f;
		float m_MouseY = 0.0f;
	};

	class MouseScrolledEvent : public Event
	{
	public:
		MouseScrolledEvent(float xOffset, float yOffset)
			: m_XOffset(xOffset)
			, m_YOffset(yOffset)
		{
		}

		float GetXOffset() const { return m_XOffset; }
		float GetYOffset() const { return m_YOffset; }

		BR_EVENT_CLASS_TYPE(MouseScrolled)
		BR_EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)

	private:
		float m_XOffset = 0.0f;
		float m_YOffset = 0.0f;
	};

	class MouseButtonEvent : public Event
	{
	public:
		MouseCode GetMouseButton() const { return m_Button; }

		BR_EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput | EventCategoryMouseButton)

	protected:
		explicit MouseButtonEvent(MouseCode button)
			: m_Button(button)
		{
		}

		MouseCode m_Button;
	};

	class MouseButtonPressedEvent : public MouseButtonEvent
	{
	public:
		explicit MouseButtonPressedEvent(MouseCode button)
			: MouseButtonEvent(button)
		{
		}

		BR_EVENT_CLASS_TYPE(MouseButtonPressed)
	};

	class MouseButtonReleasedEvent : public MouseButtonEvent
	{
	public:
		explicit MouseButtonReleasedEvent(MouseCode button)
			: MouseButtonEvent(button)
		{
		}

		BR_EVENT_CLASS_TYPE(MouseButtonReleased)
	};

}
