#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Basalt::TextEncoding {

	// Pure checks on UTF-8 and UTF-16 text, so that rules only Windows' UTF-16 file names need are tested on every platform.

	// Length (1 to 4) of the well-formed UTF-8 sequence at text[index], 0 when none starts there: a stray continuation
	// byte, a cut sequence, an overlong form, an encoded surrogate or a value above U+10FFFF.
	inline size_t Utf8SequenceLength(std::string_view text, size_t index)
	{
		const auto lead = static_cast<unsigned char>(text[index]);
		size_t length = 0;
		if (lead < 0x80)
			length = 1;
		else if (lead >= 0xC2 && lead <= 0xDF)
			length = 2;
		else if (lead >= 0xE0 && lead <= 0xEF)
			length = 3;
		else if (lead >= 0xF0 && lead <= 0xF4)
			length = 4;
		if (length == 0 || index + length > text.size())
			return 0;
		for (size_t i = 1; i < length; i++)
			if ((static_cast<unsigned char>(text[index + i]) & 0xC0) != 0x80)
				return 0;
		if (length >= 3)
		{
			const auto second = static_cast<unsigned char>(text[index + 1]);
			if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second >= 0xA0) || (lead == 0xF0 && second < 0x90) || (lead == 0xF4 && second >= 0x90))
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

	// UTF-8 text with every byte outside a well-formed sequence written as \xFF.
	inline std::string EscapeInvalidUtf8(std::string_view text)
	{
		constexpr std::string_view Digits = "0123456789ABCDEF";
		std::string result;
		result.reserve(text.size());
		for (size_t i = 0; i < text.size();)
		{
			if (const size_t length = Utf8SequenceLength(text, i); length != 0)
			{
				result.append(text.substr(i, length));
				i += length;
				continue;
			}
			const auto byte = static_cast<unsigned char>(text[i]);
			result.append("\\x").push_back(Digits[byte >> 4]);
			result.push_back(Digits[byte & 0xF]);
			i++;
		}
		return result;
	}

	// Length (1 or 2) of the well-formed UTF-16 sequence at text[index], 0 for an unpaired surrogate. Char is char16_t or,
	// on Windows, wchar_t.
	template<typename Char>
	size_t Utf16SequenceLength(std::basic_string_view<Char> text, size_t index)
	{
		const auto unit = static_cast<uint32_t>(text[index]);
		if (unit < 0xD800 || unit > 0xDFFF)
			return 1;
		if (unit > 0xDBFF || index + 1 == text.size())
			return 0;
		const auto next = static_cast<uint32_t>(text[index + 1]);
		return next >= 0xDC00 && next <= 0xDFFF ? 2 : 0;
	}

	template<typename Char>
	bool IsValidUtf16(std::basic_string_view<Char> text)
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
	std::basic_string<Char> EscapeUnpairedSurrogates(std::basic_string_view<Char> text)
	{
		constexpr std::string_view Digits = "0123456789ABCDEF";
		std::basic_string<Char> result;
		result.reserve(text.size());
		for (size_t i = 0; i < text.size();)
		{
			if (const size_t length = Utf16SequenceLength(text, i); length != 0)
			{
				result.append(text.substr(i, length));
				i += length;
				continue;
			}
			const auto unit = static_cast<uint32_t>(text[i]);
			result.push_back(static_cast<Char>('\\'));
			result.push_back(static_cast<Char>('u'));
			for (int shift = 12; shift >= 0; shift -= 4)
				result.push_back(static_cast<Char>(Digits[(unit >> shift) & 0xF]));
			i++;
		}
		return result;
	}

}
