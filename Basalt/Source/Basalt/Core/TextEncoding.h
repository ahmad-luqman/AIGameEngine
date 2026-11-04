#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Basalt::TextEncoding {

	// Pure checks and escapes on UTF-8 and UTF-16 text, so the rules behind Windows' UTF-16 file names are tested on every
	// platform.

	// Length (1 to 4) of the well-formed UTF-8 sequence at text[index], 0 when none starts there: past the end, a stray
	// continuation byte, a cut sequence, an overlong form, an encoded surrogate or a value above U+10FFFF. The limits on the
	// second byte are those of the Unicode standard's table of well-formed sequences.
	inline size_t Utf8SequenceLength(std::string_view text, size_t index)
	{
		if (index >= text.size())
			return 0;
		const auto lead = static_cast<unsigned char>(text[index]);
		if (lead < 0x80)
			return 1;
		size_t length = 0;
		unsigned char lowest = 0x80;
		unsigned char highest = 0xBF;
		if (lead >= 0xC2 && lead <= 0xDF)
			length = 2;
		else if (lead >= 0xE0 && lead <= 0xEF)
		{
			length = 3;
			if (lead == 0xE0)
				lowest = 0xA0;
			else if (lead == 0xED)
				highest = 0x9F;
		}
		else if (lead >= 0xF0 && lead <= 0xF4)
		{
			length = 4;
			if (lead == 0xF0)
				lowest = 0x90;
			else if (lead == 0xF4)
				highest = 0x8F;
		}
		if (length == 0 || length > text.size() - index)
			return 0;
		const auto second = static_cast<unsigned char>(text[index + 1]);
		if (second < lowest || second > highest)
			return 0;
		for (size_t i = 2; i < length; i++)
		{
			const auto next = static_cast<unsigned char>(text[index + i]);
			if (next < 0x80 || next > 0xBF)
				return 0;
		}
		return length;
	}

	inline bool IsValidUtf8(std::string_view text)
	{
		for (size_t i = 0; i < text.size();)
		{
			const size_t length = Utf8SequenceLength(text, i);
			if (length == 0)
				return false;
			i += length;
		}
		return true;
	}

	// Appends a backslash, kind and value in `digits` upper-case hex digits (\xFF, \uD800).
	template<typename Char>
	void AppendHexEscape(std::basic_string<Char>& result, char kind, uint32_t value, int digits)
	{
		constexpr std::string_view HexDigits = "0123456789ABCDEF";
		result.push_back(static_cast<Char>('\\'));
		result.push_back(static_cast<Char>(kind));
		for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
			result.push_back(static_cast<Char>(HexDigits[(value >> shift) & 0xF]));
	}

	// UTF-8 text with every byte outside a well-formed sequence written as \xFF.
	inline std::string EscapeInvalidUtf8(std::string_view text)
	{
		std::string result;
		result.reserve(text.size());
		for (size_t i = 0; i < text.size();)
		{
			if (const size_t length = Utf8SequenceLength(text, i); length != 0)
			{
				result.append(text, i, length);
				i += length;
				continue;
			}
			AppendHexEscape(result, 'x', static_cast<unsigned char>(text[i]), 2);
			i++;
		}
		return result;
	}

	// Length (1 or 2) of the well-formed UTF-16 sequence at text[index], 0 past the end and for an unpaired surrogate. Char
	// is char16_t or, on Windows, wchar_t.
	template<typename Char>
		requires(sizeof(Char) == 2)
	size_t Utf16SequenceLength(const std::basic_string<Char>& text, size_t index)
	{
		if (index >= text.size())
			return 0;
		const auto unit = static_cast<uint32_t>(text[index]);
		if (unit < 0xD800 || unit > 0xDFFF)
			return 1;
		if (unit > 0xDBFF || index + 1 == text.size())
			return 0;
		const auto next = static_cast<uint32_t>(text[index + 1]);
		return next >= 0xDC00 && next <= 0xDFFF ? 2 : 0;
	}

	template<typename Char>
		requires(sizeof(Char) == 2)
	bool IsValidUtf16(const std::basic_string<Char>& text)
	{
		for (size_t i = 0; i < text.size();)
		{
			const size_t length = Utf16SequenceLength(text, i);
			if (length == 0)
				return false;
			i += length;
		}
		return true;
	}

	// UTF-16 text with every unpaired surrogate written as \uD800.
	template<typename Char>
		requires(sizeof(Char) == 2)
	std::basic_string<Char> EscapeUnpairedSurrogates(const std::basic_string<Char>& text)
	{
		std::basic_string<Char> result;
		result.reserve(text.size());
		for (size_t i = 0; i < text.size();)
		{
			if (const size_t length = Utf16SequenceLength(text, i); length != 0)
			{
				result.append(text, i, length);
				i += length;
				continue;
			}
			AppendHexEscape(result, 'u', static_cast<uint32_t>(text[i]), 4);
			i++;
		}
		return result;
	}

}
