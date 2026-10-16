#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Basalt {

	// 64-bit FNV-1a: stable across platforms, compilers and runs (state hashes, cache file names); not for security.
	inline uint64_t HashFnv1a64(std::string_view text)
	{
		uint64_t hash = 14695981039346656037ull;
		for (const char c : text)
		{
			hash ^= static_cast<unsigned char>(c);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	// 16 lowercase hex digits, zero-padded.
	inline std::string ToHex64(uint64_t value)
	{
		static constexpr char Digits[] = "0123456789abcdef";
		std::string text(16, '0');
		for (size_t i = 16; i-- > 0; value >>= 4)
			text[i] = Digits[value & 0xF];
		return text;
	}

}
