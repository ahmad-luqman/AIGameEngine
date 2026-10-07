#pragma once

#include <cstdint>
#include <functional>

namespace Basalt {

	// 64-bit random identifier used for entities and assets. 0 is reserved as "invalid".
	class UUID
	{
	public:
		UUID();
		constexpr UUID(uint64_t uuid)
			: m_UUID(uuid)
		{
		}

		constexpr bool IsValid() const { return m_UUID != 0; }
		constexpr operator uint64_t() const { return m_UUID; }

	private:
		uint64_t m_UUID = 0;
	};

}

template<>
struct std::hash<Basalt::UUID>
{
	size_t operator()(const Basalt::UUID& uuid) const noexcept { return std::hash<uint64_t>()(static_cast<uint64_t>(uuid)); }
};
