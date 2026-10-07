#pragma once

#include "Basalt/Core/Base.h"

#include <string>

namespace Basalt {

	enum class EventType
	{
		None = 0,
		WindowClose,
		WindowResize,
		WindowFocus,
		WindowLostFocus,
		WindowDrop,
		KeyPressed,
		KeyReleased,
		KeyTyped,
		MouseButtonPressed,
		MouseButtonReleased,
		MouseMoved,
		MouseScrolled
	};

	enum EventCategory : uint32_t
	{
		EventCategoryNone = 0,
		EventCategoryApplication = BIT(0),
		EventCategoryInput = BIT(1),
		EventCategoryKeyboard = BIT(2),
		EventCategoryMouse = BIT(3),
		EventCategoryMouseButton = BIT(4)
	};

#define BS_EVENT_CLASS_TYPE(type)           \
	static EventType GetStaticType()        \
	{                                       \
		return EventType::type;             \
	}                                       \
	EventType GetEventType() const override \
	{                                       \
		return GetStaticType();             \
	}                                       \
	const char* GetName() const override    \
	{                                       \
		return #type;                       \
	}

#define BS_EVENT_CLASS_CATEGORY(category)      \
	uint32_t GetCategoryFlags() const override \
	{                                          \
		return category;                       \
	}

	// Base class for window and input events, dispatched through Application::OnEvent to layers.
	class Event
	{
	public:
		virtual ~Event() = default;

		bool Handled = false;

		virtual EventType GetEventType() const = 0;
		virtual const char* GetName() const = 0;
		virtual uint32_t GetCategoryFlags() const = 0;
		virtual std::string ToString() const { return GetName(); }

		bool IsInCategory(EventCategory category) const { return (GetCategoryFlags() & category) != 0; }
	};

	// Routes an event to a handler if its type matches. The handler returns true to mark it handled.
	class EventDispatcher
	{
	public:
		explicit EventDispatcher(Event& event)
			: m_Event(event)
		{
		}

		template<typename T, typename F>
		bool Dispatch(const F& func)
		{
			if (m_Event.GetEventType() != T::GetStaticType())
				return false;
			m_Event.Handled |= func(static_cast<T&>(m_Event));
			return true;
		}

	private:
		Event& m_Event;
	};

}
