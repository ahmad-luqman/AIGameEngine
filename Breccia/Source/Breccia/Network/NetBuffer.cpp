#include "Breccia/Network/NetBuffer.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace Breccia {

	namespace {

		template<typename T>
		void WriteLittleEndian(std::vector<uint8_t>& bytes, T value)
		{
			for (size_t i = 0; i < sizeof(T); i++)
				bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
		}

		template<typename T>
		T ReadLittleEndian(const uint8_t* data)
		{
			T value = 0;
			for (size_t i = sizeof(T); i-- > 0;)
				value = static_cast<T>((value << 8) | data[i]);
			return value;
		}

	}

	void NetWriter::WriteU32(uint32_t value)
	{
		WriteLittleEndian(m_Bytes, value);
	}

	void NetWriter::WriteU64(uint64_t value)
	{
		WriteLittleEndian(m_Bytes, value);
	}

	void NetWriter::WriteVarUInt(uint64_t value)
	{
		while (value >= 0x80)
		{
			WriteU8(static_cast<uint8_t>(value | 0x80));
			value >>= 7;
		}
		WriteU8(static_cast<uint8_t>(value));
	}

	void NetWriter::WriteFloat(float value)
	{
		WriteU32(std::bit_cast<uint32_t>(value));
	}

	void NetWriter::WriteDouble(double value)
	{
		WriteU64(std::bit_cast<uint64_t>(value));
	}

	void NetWriter::WriteString(std::string_view text)
	{
		WriteVarUInt(text.size());
		m_Bytes.insert(m_Bytes.end(), text.begin(), text.end());
	}

	void NetWriter::WriteBytes(std::span<const uint8_t> bytes)
	{
		m_Bytes.insert(m_Bytes.end(), bytes.begin(), bytes.end());
	}

	const uint8_t* NetReader::Take(size_t count)
	{
		if (m_Failed || count > m_Bytes.size() - m_Offset)
		{
			m_Failed = true;
			return nullptr;
		}
		const uint8_t* data = m_Bytes.data() + m_Offset;
		m_Offset += count;
		return data;
	}

	bool NetReader::ReadU8(uint8_t& value)
	{
		const uint8_t* data = Take(1);
		if (!data)
			return false;
		value = data[0];
		return true;
	}

	bool NetReader::ReadU32(uint32_t& value)
	{
		const uint8_t* data = Take(sizeof(value));
		if (data)
			value = ReadLittleEndian<uint32_t>(data);
		return data != nullptr;
	}

	bool NetReader::ReadU64(uint64_t& value)
	{
		const uint8_t* data = Take(sizeof(value));
		if (data)
			value = ReadLittleEndian<uint64_t>(data);
		return data != nullptr;
	}

	bool NetReader::ReadVarUInt(uint64_t& value)
	{
		value = 0;
		for (int shift = 0; shift < 70; shift += 7)
		{
			uint8_t byte = 0;
			if (!ReadU8(byte))
				return false;
			// The 10th byte holds bit 63 only: anything more would overflow.
			if (shift == 63 && byte > 1)
				break;
			value |= static_cast<uint64_t>(byte & 0x7F) << shift;
			if ((byte & 0x80) == 0)
				return true;
		}
		m_Failed = true;
		return false;
	}

	bool NetReader::ReadCount(uint32_t& value, uint64_t max)
	{
		uint64_t wide = 0;
		if (!ReadVarUInt(wide))
			return false;
		if (wide > std::min<uint64_t>(max, UINT32_MAX))
		{
			m_Failed = true;
			return false;
		}
		value = static_cast<uint32_t>(wide);
		return true;
	}

	bool NetReader::ReadFloat(float& value)
	{
		uint32_t bits = 0;
		if (!ReadU32(bits))
			return false;
		value = std::bit_cast<float>(bits);
		return true;
	}

	bool NetReader::ReadDouble(double& value)
	{
		uint64_t bits = 0;
		if (!ReadU64(bits))
			return false;
		value = std::bit_cast<double>(bits);
		return true;
	}

	bool NetReader::ReadString(std::string& text, size_t maxLength)
	{
		uint64_t length = 0;
		if (!ReadVarUInt(length))
			return false;
		if (length > maxLength)
		{
			m_Failed = true;
			return false;
		}
		const uint8_t* data = Take(static_cast<size_t>(length));
		if (!data)
			return false;
		text.assign(reinterpret_cast<const char*>(data), static_cast<size_t>(length));
		return true;
	}

	bool NetReader::ReadBytes(std::span<const uint8_t>& bytes, size_t count)
	{
		const uint8_t* data = Take(count);
		if (!data)
			return false;
		bytes = { data, count };
		return true;
	}

}
