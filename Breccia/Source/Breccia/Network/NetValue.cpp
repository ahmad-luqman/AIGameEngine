#include "Breccia/Network/NetValue.h"

#include "Breccia/Core/TextEncoding.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>

namespace Breccia::NetValue {

	namespace {

		enum class Tag : uint8_t
		{
			Null = 0,
			False,
			True,
			// A non-negative integer.
			Unsigned,
			// A negative integer n, stored as -(n + 1).
			Negative,
			Float32,
			Float64,
			String,
			Array,
			Object,
			// An array of numbers that are all exact float32 values.
			Float32Array,
			Count
		};

		// Whether the number is a float32 exactly (NaN and infinities included, which float32 holds too).
		bool IsExactFloat32(double number)
		{
			if (std::isnan(number) || std::isinf(number))
				return true;
			// Converting a finite double outside float's range is undefined behaviour, so range-check first.
			if (std::fabs(number) > static_cast<double>(std::numeric_limits<float>::max()))
				return false;
			return static_cast<double>(static_cast<float>(number)) == number;
		}

		bool IsFloat32Array(const nlohmann::json& value)
		{
			if (value.empty())
				return false;
			for (const nlohmann::json& element : value)
				if (!element.is_number_float() || !IsExactFloat32(element.get<double>()))
					return false;
			return true;
		}

		void WriteTag(NetWriter& writer, Tag tag)
		{
			writer.WriteU8(static_cast<uint8_t>(tag));
		}

		bool EncodeValue(const nlohmann::json& value, NetWriter& writer, int depth)
		{
			if (depth > MaxDepth)
				return false;
			switch (value.type())
			{
				case nlohmann::json::value_t::null:
					WriteTag(writer, Tag::Null);
					return true;
				case nlohmann::json::value_t::boolean:
					WriteTag(writer, value.get<bool>() ? Tag::True : Tag::False);
					return true;
				case nlohmann::json::value_t::number_unsigned:
					WriteTag(writer, Tag::Unsigned);
					writer.WriteVarUInt(value.get<uint64_t>());
					return true;
				case nlohmann::json::value_t::number_integer:
				{
					const int64_t number = value.get<int64_t>();
					if (number >= 0)
					{
						WriteTag(writer, Tag::Unsigned);
						writer.WriteVarUInt(static_cast<uint64_t>(number));
					}
					else
					{
						WriteTag(writer, Tag::Negative);
						// -(n + 1) cannot overflow for any negative int64 (unlike -n for the minimum).
						writer.WriteVarUInt(static_cast<uint64_t>(-(number + 1)));
					}
					return true;
				}
				case nlohmann::json::value_t::number_float:
				{
					const double number = value.get<double>();
					if (IsExactFloat32(number))
					{
						WriteTag(writer, Tag::Float32);
						writer.WriteFloat(static_cast<float>(number));
					}
					else
					{
						WriteTag(writer, Tag::Float64);
						writer.WriteDouble(number);
					}
					return true;
				}
				case nlohmann::json::value_t::string:
				{
					const std::string& text = value.get_ref<const std::string&>();
					if (!TextEncoding::IsValidUtf8(text))
						return false;
					WriteTag(writer, Tag::String);
					writer.WriteString(text);
					return true;
				}
				case nlohmann::json::value_t::array:
					if (IsFloat32Array(value))
					{
						WriteTag(writer, Tag::Float32Array);
						writer.WriteVarUInt(value.size());
						for (const nlohmann::json& element : value)
							writer.WriteFloat(static_cast<float>(element.get<double>()));
						return true;
					}
					WriteTag(writer, Tag::Array);
					writer.WriteVarUInt(value.size());
					for (const nlohmann::json& element : value)
						if (!EncodeValue(element, writer, depth + 1))
							return false;
					return true;
				case nlohmann::json::value_t::object:
					WriteTag(writer, Tag::Object);
					writer.WriteVarUInt(value.size());
					for (const auto& [key, element] : value.items())
					{
						if (!TextEncoding::IsValidUtf8(key))
							return false;
						writer.WriteString(key);
						if (!EncodeValue(element, writer, depth + 1))
							return false;
					}
					return true;
				case nlohmann::json::value_t::binary:
				case nlohmann::json::value_t::discarded:
					return false;
			}
			return false;
		}

		bool ReadText(NetReader& reader, std::string& text)
		{
			if (!reader.ReadString(text, reader.GetRemaining()))
				return false;
			if (!TextEncoding::IsValidUtf8(text))
			{
				reader.Fail();
				return false;
			}
			return true;
		}

		bool DecodeValue(NetReader& reader, nlohmann::json& value, int depth)
		{
			if (depth > MaxDepth)
			{
				reader.Fail();
				return false;
			}
			uint8_t tagByte = 0;
			if (!reader.ReadU8(tagByte))
				return false;
			switch (static_cast<Tag>(tagByte))
			{
				case Tag::Null:
					value = nullptr;
					return true;
				case Tag::False:
					value = false;
					return true;
				case Tag::True:
					value = true;
					return true;
				case Tag::Unsigned:
				{
					uint64_t number = 0;
					if (!reader.ReadVarUInt(number))
						return false;
					value = number;
					return true;
				}
				case Tag::Negative:
				{
					uint64_t magnitude = 0;
					if (!reader.ReadVarUInt(magnitude))
						return false;
					// -(magnitude + 1) must fit in int64: magnitude at most INT64_MAX.
					if (magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
					{
						reader.Fail();
						return false;
					}
					value = -static_cast<int64_t>(magnitude) - 1;
					return true;
				}
				case Tag::Float32:
				{
					float number = 0.0f;
					if (!reader.ReadFloat(number))
						return false;
					value = static_cast<double>(number);
					return true;
				}
				case Tag::Float64:
				{
					double number = 0.0;
					if (!reader.ReadDouble(number))
						return false;
					value = number;
					return true;
				}
				case Tag::String:
				{
					std::string text;
					if (!ReadText(reader, text))
						return false;
					value = std::move(text);
					return true;
				}
				case Tag::Float32Array:
				{
					uint32_t count = 0;
					// Four bytes per element, and an empty one is written as an ordinary array.
					if (!reader.ReadCount(count, static_cast<uint32_t>(std::min<size_t>(reader.GetRemaining() / 4, std::numeric_limits<uint32_t>::max()))) || count == 0)
					{
						reader.Fail();
						return false;
					}
					value = nlohmann::json::array();
					value.get_ref<nlohmann::json::array_t&>().reserve(count);
					for (uint32_t i = 0; i < count; i++)
					{
						float number = 0.0f;
						if (!reader.ReadFloat(number))
							return false;
						value.push_back(static_cast<double>(number));
					}
					return true;
				}
				case Tag::Array:
				{
					uint32_t count = 0;
					// Every element takes at least its tag byte.
					if (!reader.ReadCount(count, static_cast<uint32_t>(std::min<size_t>(reader.GetRemaining(), std::numeric_limits<uint32_t>::max()))))
						return false;
					value = nlohmann::json::array();
					value.get_ref<nlohmann::json::array_t&>().reserve(count);
					for (uint32_t i = 0; i < count; i++)
					{
						nlohmann::json element;
						if (!DecodeValue(reader, element, depth + 1))
							return false;
						value.push_back(std::move(element));
					}
					return true;
				}
				case Tag::Object:
				{
					uint32_t count = 0;
					// Every entry takes at least a key length byte and a tag byte.
					if (!reader.ReadCount(count, static_cast<uint32_t>(std::min<size_t>(reader.GetRemaining() / 2, std::numeric_limits<uint32_t>::max()))))
						return false;
					value = nlohmann::json::object();
					for (uint32_t i = 0; i < count; i++)
					{
						std::string key;
						if (!ReadText(reader, key))
							return false;
						nlohmann::json element;
						if (!DecodeValue(reader, element, depth + 1))
							return false;
						// A repeated key would silently replace the first: two encodings of one object must not exist.
						if (!value.emplace(std::move(key), std::move(element)).second)
						{
							reader.Fail();
							return false;
						}
					}
					return true;
				}
				case Tag::Count:
					break;
			}
			reader.Fail();
			return false;
		}

	}

	bool Encode(const nlohmann::json& value, NetWriter& writer)
	{
		return EncodeValue(value, writer, 0);
	}

	bool Decode(NetReader& reader, nlohmann::json& value)
	{
		return DecodeValue(reader, value, 0) && reader.Ok();
	}

}
