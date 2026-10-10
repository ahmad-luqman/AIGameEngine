#include "Basalt/Physics/JoltUtils.h"

#include "Basalt/Core/Log.h"

#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace Basalt::PhysicsInternal {

	namespace {

		std::mutex s_JoltMutex;
		uint32_t s_JoltUsers = 0;

		void JoltTrace(const char* format, ...)
		{
			char buffer[1024];
			va_list args;
			va_start(args, format);
			std::vsnprintf(buffer, sizeof(buffer), format, args);
			va_end(args);
			BS_CORE_TRACE("[Jolt] {}", buffer);
		}

	}

	void AcquireJolt()
	{
		std::scoped_lock lock(s_JoltMutex);
		if (s_JoltUsers++ > 0)
			return;
		JPH::RegisterDefaultAllocator();
		JPH::Trace = JoltTrace;
		JPH::Factory::sInstance = new JPH::Factory();
		JPH::RegisterTypes();
	}

	void ReleaseJolt()
	{
		std::scoped_lock lock(s_JoltMutex);
		if (--s_JoltUsers > 0)
			return;
		JPH::UnregisterTypes();
		delete JPH::Factory::sInstance;
		JPH::Factory::sInstance = nullptr;
	}

}
