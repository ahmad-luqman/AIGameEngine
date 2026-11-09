#pragma once

#include "Breccia/Network/NetBuffer.h"

#include <nlohmann/json_fwd.hpp>

namespace Breccia {

	// A compact binary form of the JSON that ComponentRegistry reads and writes, so replicated fields and spawned
	// entities keep the engine's one data path without sending text. Each value is a tag byte and its payload: integers
	// as LEB128, numbers as float32 when that is exact (float64 otherwise), arrays whose elements are all exact float32
	// numbers (vectors, colors, rotations) as one tag, a count and the raw floats.
	namespace NetValue {

		// Deeper values fail to decode (and to encode), so a hostile packet cannot overflow the stack. Component data
		// nests fewer than 10 levels.
		inline constexpr int MaxDepth = 32;

		// False (with nothing useful written) for a value no decoder would accept: binary JSON, a string that is not
		// UTF-8, nesting deeper than MaxDepth.
		bool Encode(const nlohmann::json& value, NetWriter& writer);
		// Reads one value. Fails, failing the reader, on an unknown tag, a truncated or overlong payload, text that is
		// not UTF-8, a duplicate object key or nesting deeper than MaxDepth. Counts are checked against the remaining
		// bytes before anything is allocated.
		bool Decode(NetReader& reader, nlohmann::json& value);

	}

}
