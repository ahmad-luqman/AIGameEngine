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
		std::once_flag s_AllocatorInitialized;
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

#ifdef JPH_ENABLE_ASSERTS
		// Jolt's default handler reports nothing before its breakpoint, which left a bare SIGTRAP. A failed Jolt assert
		// means Basalt misused Jolt (invalid data or a broken job/barrier contract), so it is reported like a failed
		// BS_CORE_ASSERT. Jolt has asserts only in builds without NDEBUG (Debug), so the hook exists only there;
		// Basalt's Release keeps BS_CORE_ASSERT but has no Jolt asserts.
		bool JoltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line)
		{
			if (message)
				BS_CORE_CRITICAL("[Jolt] Assertion '{}' failed at {}:{}: {}", expression, file, line, message);
			else
				BS_CORE_CRITICAL("[Jolt] Assertion '{}' failed at {}:{}", expression, file, line);
			// Jolt then breaks (JPH_BREAKPOINT), as BS_CORE_ASSERT does.
			return true;
		}
#endif

	}

	void AcquireJolt()
	{
		std::scoped_lock lock(s_JoltMutex);
		if (s_JoltUsers++ > 0)
			return;
		// Completed jobs can be freed after a world stops. Never rewrite allocator callbacks
		// while those worker deletions read them during a later world's initialization.
		std::call_once(s_AllocatorInitialized, [] {
			JPH::RegisterDefaultAllocator();
			JPH::Trace = JoltTrace;
#ifdef JPH_ENABLE_ASSERTS
			JPH::AssertFailed = JoltAssertFailed;
#endif
		});
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
