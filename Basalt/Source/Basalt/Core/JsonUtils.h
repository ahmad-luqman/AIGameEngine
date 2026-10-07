#pragma once

#include <nlohmann/json.hpp>

#include <cmath>
#include <exception>
#include <limits>
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

}
