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

namespace Basalt {

	// Untrusted JSON (files, automation requests) may not nest deeper than this. Copying and dumping a
	// json value recurse once per level, so unbounded nesting could overflow the stack. Real Basalt data
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

	// Like ParseJson, and says why it failed: the parser's message with the byte position of the error, or the depth.
	inline nlohmann::json ParseJson(std::string_view text, std::string& outError)
	{
		struct TooDeep : std::exception
		{
		};
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
		catch (const nlohmann::json::parse_error& e)
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
