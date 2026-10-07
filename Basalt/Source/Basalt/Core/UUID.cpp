#include "Basalt/Core/UUID.h"

#include <random>

namespace Basalt {

	namespace {

		uint64_t GenerateRandomUUID()
		{
			thread_local std::mt19937_64 engine(std::random_device{}());
			thread_local std::uniform_int_distribution<uint64_t> distribution(1, UINT64_MAX);
			return distribution(engine);
		}

	}

	UUID::UUID()
		: m_UUID(GenerateRandomUUID())
	{
	}

}
