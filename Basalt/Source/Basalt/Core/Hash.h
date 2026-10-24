#pragma once

#include <cstddef>
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

	// FNV-1a's step over 8-byte little-endian words, then over the remaining bytes, with a final avalanche: about eight
	// times faster than HashFnv1a64 on large blobs (archive integrity checks). Each step is a bijection of the state, so
	// any change to a single word always changes the hash. Stable across platforms; not for security.
	inline uint64_t HashBytes64(const void* data, size_t size)
	{
		const auto* bytes = static_cast<const unsigned char*>(data);
		uint64_t hash = 14695981039346656037ull ^ size;
		size_t i = 0;
		for (; i + 8 <= size; i += 8)
		{
			uint64_t word = 0;
			for (size_t b = 0; b < 8; b++)
				word |= static_cast<uint64_t>(bytes[i + b]) << (8 * b);
			hash ^= word;
			hash *= 1099511628211ull;
			hash ^= hash >> 29;
		}
		for (; i < size; i++)
		{
			hash ^= bytes[i];
			hash *= 1099511628211ull;
		}
		hash ^= hash >> 33;
		hash *= 0xff51afd7ed558ccdull;
		hash ^= hash >> 33;
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
