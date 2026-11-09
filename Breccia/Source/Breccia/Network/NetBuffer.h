#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Breccia {

	// Appends little-endian fixed-width values, LEB128 variable-length integers and length-prefixed strings to a byte
	// vector: the encoding of every network message (Network/NetProtocol).
	class NetWriter
	{
	public:
		void WriteU8(uint8_t value) { m_Bytes.push_back(value); }
		void WriteU32(uint32_t value);
		void WriteU64(uint64_t value);
		// Unsigned LEB128: 1 byte below 128, at most 10 bytes.
		void WriteVarUInt(uint64_t value);
		void WriteFloat(float value);
		void WriteDouble(double value);
		// A VarUInt byte count followed by the bytes (UTF-8 text is not checked here).
		void WriteString(std::string_view text);
		void WriteBytes(std::span<const uint8_t> bytes);

		const std::vector<uint8_t>& GetBytes() const { return m_Bytes; }
		std::vector<uint8_t> TakeBytes() { return std::move(m_Bytes); }

	private:
		std::vector<uint8_t> m_Bytes;
	};

	// Reads what NetWriter writes from bytes that came from the network, so nothing about them is trusted: every read is
	// bounds-checked, and the first failure (past the end, an overlong integer, a length beyond the remaining bytes or a
	// caller's limit) makes this and every later read fail. Callers check Ok() (or a read's result) and never act on
	// values read after a failure.
	class NetReader
	{
	public:
		explicit NetReader(std::span<const uint8_t> bytes)
			: m_Bytes(bytes)
		{
		}
		// The reader keeps a view: a temporary vector would be gone before the first read.
		explicit NetReader(std::vector<uint8_t>&&) = delete;

		bool ReadU8(uint8_t& value);
		bool ReadU32(uint32_t& value);
		bool ReadU64(uint64_t& value);
		// Fails on more than 10 bytes, or on a 10th byte that would overflow 64 bits. Longer forms of a value than needed
		// (0x80 0x00 for 0) are accepted: nothing compares encodings it received.
		bool ReadVarUInt(uint64_t& value);
		// A VarUInt that must not exceed `max` (counts and indices; a max beyond 32 bits means UINT32_MAX), read into 32
		// bits.
		bool ReadCount(uint32_t& value, uint64_t max);
		bool ReadFloat(float& value);
		bool ReadDouble(double& value);
		// Fails when the length exceeds maxLength or the remaining bytes.
		bool ReadString(std::string& text, size_t maxLength);
		bool ReadBytes(std::span<const uint8_t>& bytes, size_t count);

		// Makes this and every later read fail (a value that decoded but is invalid).
		void Fail() { m_Failed = true; }
		bool Ok() const { return !m_Failed; }
		size_t GetRemaining() const { return m_Failed ? 0 : m_Bytes.size() - m_Offset; }
		bool AtEnd() const { return !m_Failed && m_Offset == m_Bytes.size(); }

	private:
		// The next `count` bytes, or null (and failed) when fewer remain.
		const uint8_t* Take(size_t count);

		std::span<const uint8_t> m_Bytes;
		size_t m_Offset = 0;
		bool m_Failed = false;
	};

}
