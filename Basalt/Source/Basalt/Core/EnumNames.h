#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>

namespace Basalt {

	// The enum value whose name (in a table indexed by enum value) equals name; nullopt when none does. Shared
	// by the component registry and the Lua bindings so enum names are matched one way (exactly, case included).
	template<typename E, size_t N>
	std::optional<E> EnumFromName(const char* const (&names)[N], std::string_view name)
	{
		static_assert(std::is_enum_v<E>);
		for (size_t i = 0; i < N; i++)
		{
			if (name == names[i])
				return static_cast<E>(i);
		}
		return std::nullopt;
	}

	// The name of an enum value in such a table; nullptr for a value outside it (C++ code can store one).
	template<typename E, size_t N>
	const char* EnumNameOrNull(const char* const (&names)[N], E value)
	{
		static_assert(std::is_enum_v<E>);
		const auto index = static_cast<size_t>(value);
		return index < N ? names[index] : nullptr;
	}

}
