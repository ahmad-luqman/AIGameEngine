#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>

namespace Breccia {

	// Untrusted JSON (files, automation requests) may not nest deeper than this. Copying and dumping a
	// json value recurse once per level, so unbounded nesting could overflow the stack. Real Breccia data
	// nests fewer than 10 levels.
	inline constexpr int MaxJsonDepth = 128;

	// Parses JSON without throwing. Returns a discarded value (check is_discarded()) on a syntax error or
	// when the document nests deeper than MaxJsonDepth.
	inline nlohmann::json ParseJson(std::string_view text)
	{
		// Thrown from the parser callback to stop at the first too-deep value; never escapes ParseJson.
		struct TooDeep : std::exception
		{
		};
		try
		{
			return nlohmann::json::parse(
				text, [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
					// The outermost container is depth 0, so this allows exactly MaxJsonDepth levels.
					if (depth >= MaxJsonDepth)
						throw TooDeep{};
					return true;
				},
				false);
		}
		catch (const TooDeep&)
		{
			return nlohmann::json(nlohmann::json::value_t::discarded);
		}
	}

	// Like ParseJson, and says why it failed (outError is cleared otherwise): the parser's message with the position of
	// the error (a syntax error, or a number out of range), or the depth.
	inline nlohmann::json ParseJson(std::string_view text, std::string& outError)
	{
		struct TooDeep : std::exception
		{
		};
		outError.clear();
		try
		{
			return nlohmann::json::parse(text, [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
				if (depth >= MaxJsonDepth)
					throw TooDeep{};
				return true;
			});
		}
		catch (const TooDeep&)
		{
			outError = "the JSON nests deeper than " + std::to_string(MaxJsonDepth) + " levels";
		}
		catch (const nlohmann::json::exception& e)
		{
			outError = e.what();
		}
		return nlohmann::json(nlohmann::json::value_t::discarded);
	}

	// Strict readers: fails, naming the first key of `object` that is not in `allowed`, prefixed by `context`.
	inline bool RejectUnknownKeys(const nlohmann::json& object, std::initializer_list<std::string_view> allowed, std::string_view context, std::string& outError)
	{
		for (const auto& [key, value] : object.items())
		{
			if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
			{
				outError = std::string(context) + ": unknown key '" + key + "'";
				return false;
			}
		}
		return true;
	}

	// For list files such as {"Joints": [...]} (masks, joint maps): the array under `key` of `data`, an object holding
	// nothing else, with at most maxEntries entries; null with outError otherwise (`notObject` when data is not an object,
	// "<context>: unknown key ..." for another key, "<key> must be an array with at most N entries").
	inline const nlohmann::json* ReadEntryArray(const nlohmann::json& data, std::string_view key, std::string_view context, std::string_view notObject, size_t maxEntries, std::string& outError)
	{
		outError.clear();
		if (!data.is_object())
		{
			outError = notObject;
			return nullptr;
		}
		if (!RejectUnknownKeys(data, { key }, context, outError))
			return nullptr;
		// contains/at rather than find: GCC cannot see that a found iterator is not null.
		const std::string name(key);
		if (!data.contains(name) || !data.at(name).is_array() || data.at(name).size() > maxEntries)
		{
			outError = name + " must be an array with at most " + std::to_string(maxEntries) + " entries";
			return nullptr;
		}
		return &data.at(name);
	}

	// Whether an entry of such a list is an object holding only `allowed` keys; "<location> must be an object" (or the
	// unknown key) otherwise.
	inline bool RequireEntryObject(const nlohmann::json& entry, std::initializer_list<std::string_view> allowed, const std::string& location, std::string& outError)
	{
		if (!entry.is_object())
		{
			outError = location + " must be an object";
			return false;
		}
		return RejectUnknownKeys(entry, allowed, location, outError);
	}

	// entry[key] as a nonempty string, or null with "<location>.<key> must be a nonempty string".
	inline const std::string* ReadNonemptyString(const nlohmann::json& entry, std::string_view key, const std::string& location, std::string& outError)
	{
		const std::string name(key);
		if (!entry.contains(name) || !entry.at(name).is_string() || entry.at(name).get_ref<const std::string&>().empty())
		{
			outError = location + "." + name + " must be a nonempty string";
			return nullptr;
		}
		return &entry.at(name).get_ref<const std::string&>();
	}

	// Reads a JSON number into a float. JSON numbers are doubles, and converting one outside float range
	// is undefined behaviour (and would put infinities into transforms), so out-of-range values and
	// non-finite numbers are rejected. Returns false when `value` is not a usable float.
	inline bool JsonToFloat(const nlohmann::json& value, float& out)
	{
		if (!value.is_number())
			return false;
		const double number = value.get<double>();
		if (!std::isfinite(number) || std::abs(number) > static_cast<double>(std::numeric_limits<float>::max()))
			return false;
		out = static_cast<float>(number);
		return true;
	}

	// Reads a JSON integer in [0, UINT32_MAX] (nlohmann's get<uint32_t> would wrap negative and larger values).
	// Returns false when `value` is not such an integer.
	inline bool JsonToUInt32(const nlohmann::json& value, uint32_t& out)
	{
		if (value.is_number_unsigned())
		{
			if (value.get<uint64_t>() > std::numeric_limits<uint32_t>::max())
				return false;
			out = static_cast<uint32_t>(value.get<uint64_t>());
			return true;
		}
		if (!value.is_number_integer() || value.get<int64_t>() < 0 || value.get<int64_t>() > std::numeric_limits<uint32_t>::max())
			return false;
		out = static_cast<uint32_t>(value.get<int64_t>());
		return true;
	}

}
