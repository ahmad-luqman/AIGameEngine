#include "Basalt/Core/KeyCodes.h"

#include <array>
#include <cctype>
#include <utility>

namespace Basalt {

	namespace {

		constexpr std::pair<KeyCode, std::string_view> s_KeyNames[] = {
			{ KeyCode::Space, "Space" },
			{ KeyCode::Apostrophe, "Apostrophe" },
			{ KeyCode::Comma, "Comma" },
			{ KeyCode::Minus, "Minus" },
			{ KeyCode::Period, "Period" },
			{ KeyCode::Slash, "Slash" },
			{ KeyCode::D0, "D0" },
			{ KeyCode::D1, "D1" },
			{ KeyCode::D2, "D2" },
			{ KeyCode::D3, "D3" },
			{ KeyCode::D4, "D4" },
			{ KeyCode::D5, "D5" },
			{ KeyCode::D6, "D6" },
			{ KeyCode::D7, "D7" },
			{ KeyCode::D8, "D8" },
			{ KeyCode::D9, "D9" },
			{ KeyCode::Semicolon, "Semicolon" },
			{ KeyCode::Equal, "Equal" },
			{ KeyCode::A, "A" },
			{ KeyCode::B, "B" },
			{ KeyCode::C, "C" },
			{ KeyCode::D, "D" },
			{ KeyCode::E, "E" },
			{ KeyCode::F, "F" },
			{ KeyCode::G, "G" },
			{ KeyCode::H, "H" },
			{ KeyCode::I, "I" },
			{ KeyCode::J, "J" },
			{ KeyCode::K, "K" },
			{ KeyCode::L, "L" },
			{ KeyCode::M, "M" },
			{ KeyCode::N, "N" },
			{ KeyCode::O, "O" },
			{ KeyCode::P, "P" },
			{ KeyCode::Q, "Q" },
			{ KeyCode::R, "R" },
			{ KeyCode::S, "S" },
			{ KeyCode::T, "T" },
			{ KeyCode::U, "U" },
			{ KeyCode::V, "V" },
			{ KeyCode::W, "W" },
			{ KeyCode::X, "X" },
			{ KeyCode::Y, "Y" },
			{ KeyCode::Z, "Z" },
			{ KeyCode::LeftBracket, "LeftBracket" },
			{ KeyCode::Backslash, "Backslash" },
			{ KeyCode::RightBracket, "RightBracket" },
			{ KeyCode::GraveAccent, "GraveAccent" },
			{ KeyCode::Escape, "Escape" },
			{ KeyCode::Enter, "Enter" },
			{ KeyCode::Tab, "Tab" },
			{ KeyCode::Backspace, "Backspace" },
			{ KeyCode::Insert, "Insert" },
			{ KeyCode::Delete, "Delete" },
			{ KeyCode::Right, "Right" },
			{ KeyCode::Left, "Left" },
			{ KeyCode::Down, "Down" },
			{ KeyCode::Up, "Up" },
			{ KeyCode::PageUp, "PageUp" },
			{ KeyCode::PageDown, "PageDown" },
			{ KeyCode::Home, "Home" },
			{ KeyCode::End, "End" },
			{ KeyCode::CapsLock, "CapsLock" },
			{ KeyCode::ScrollLock, "ScrollLock" },
			{ KeyCode::NumLock, "NumLock" },
			{ KeyCode::PrintScreen, "PrintScreen" },
			{ KeyCode::Pause, "Pause" },
			{ KeyCode::F1, "F1" },
			{ KeyCode::F2, "F2" },
			{ KeyCode::F3, "F3" },
			{ KeyCode::F4, "F4" },
			{ KeyCode::F5, "F5" },
			{ KeyCode::F6, "F6" },
			{ KeyCode::F7, "F7" },
			{ KeyCode::F8, "F8" },
			{ KeyCode::F9, "F9" },
			{ KeyCode::F10, "F10" },
			{ KeyCode::F11, "F11" },
			{ KeyCode::F12, "F12" },
			{ KeyCode::KP0, "KP0" },
			{ KeyCode::KP1, "KP1" },
			{ KeyCode::KP2, "KP2" },
			{ KeyCode::KP3, "KP3" },
			{ KeyCode::KP4, "KP4" },
			{ KeyCode::KP5, "KP5" },
			{ KeyCode::KP6, "KP6" },
			{ KeyCode::KP7, "KP7" },
			{ KeyCode::KP8, "KP8" },
			{ KeyCode::KP9, "KP9" },
			{ KeyCode::KPDecimal, "KPDecimal" },
			{ KeyCode::KPDivide, "KPDivide" },
			{ KeyCode::KPMultiply, "KPMultiply" },
			{ KeyCode::KPSubtract, "KPSubtract" },
			{ KeyCode::KPAdd, "KPAdd" },
			{ KeyCode::KPEnter, "KPEnter" },
			{ KeyCode::KPEqual, "KPEqual" },
			{ KeyCode::LeftShift, "LeftShift" },
			{ KeyCode::LeftControl, "LeftControl" },
			{ KeyCode::LeftAlt, "LeftAlt" },
			{ KeyCode::LeftSuper, "LeftSuper" },
			{ KeyCode::RightShift, "RightShift" },
			{ KeyCode::RightControl, "RightControl" },
			{ KeyCode::RightAlt, "RightAlt" },
			{ KeyCode::RightSuper, "RightSuper" },
			{ KeyCode::Menu, "Menu" },
		};

		bool EqualsIgnoreCase(std::string_view a, std::string_view b)
		{
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); i++)
			{
				if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
					return false;
			}
			return true;
		}

	}

	std::string_view KeyCodeToString(KeyCode key)
	{
		for (const auto& [code, name] : s_KeyNames)
		{
			if (code == key)
				return name;
		}
		return "Unknown";
	}

	std::optional<KeyCode> KeyCodeFromString(std::string_view name)
	{
		for (const auto& [code, keyName] : s_KeyNames)
		{
			if (EqualsIgnoreCase(keyName, name))
				return code;
		}
		return std::nullopt;
	}

}
