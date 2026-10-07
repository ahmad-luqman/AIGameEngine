#pragma once

#include "Basalt/Core/KeyCodes.h"
#include "Basalt/Events/Event.h"

namespace Basalt {

	class KeyEvent : public Event
	{
	public:
		KeyCode GetKeyCode() const { return m_KeyCode; }

		BS_EVENT_CLASS_CATEGORY(EventCategoryKeyboard | EventCategoryInput)

	protected:
		explicit KeyEvent(KeyCode keyCode)
			: m_KeyCode(keyCode)
		{
		}

		KeyCode m_KeyCode;
	};

	class KeyPressedEvent : public KeyEvent
	{
	public:
		KeyPressedEvent(KeyCode keyCode, bool isRepeat = false)
			: KeyEvent(keyCode)
			, m_IsRepeat(isRepeat)
		{
		}

		bool IsRepeat() const { return m_IsRepeat; }

		std::string ToString() const override
		{
			return "KeyPressedEvent: " + std::to_string(static_cast<int>(m_KeyCode)) + (m_IsRepeat ? " (repeat)" : "");
		}

		BS_EVENT_CLASS_TYPE(KeyPressed)

	private:
		bool m_IsRepeat = false;
	};

	class KeyReleasedEvent : public KeyEvent
	{
	public:
		explicit KeyReleasedEvent(KeyCode keyCode)
			: KeyEvent(keyCode)
		{
		}

		BS_EVENT_CLASS_TYPE(KeyReleased)
	};

	class KeyTypedEvent : public Event
	{
	public:
		explicit KeyTypedEvent(uint32_t codepoint)
			: m_Codepoint(codepoint)
		{
		}

		uint32_t GetCodepoint() const { return m_Codepoint; }

		BS_EVENT_CLASS_TYPE(KeyTyped)
		BS_EVENT_CLASS_CATEGORY(EventCategoryKeyboard | EventCategoryInput)

	private:
		uint32_t m_Codepoint = 0;
	};

}
