#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace Basalt {

	// The enum value whose name (in a table indexed by enum value) equals name; nullopt when none does. Shared
	// by the component registry and the Lua bindings so enum names are matched one way (exactly, case included).
	template<typename E, size_t N>
	std::optional<E> EnumFromName(const char* const (&names)[N], std::string_view name)
	{
		for (size_t i = 0; i < N; i++)
		{
			if (name == names[i])
				return static_cast<E>(i);
		}
		return std::nullopt;
	}

}
