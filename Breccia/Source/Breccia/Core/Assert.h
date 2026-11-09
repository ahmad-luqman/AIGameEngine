#pragma once

#include "Breccia/Core/Base.h"
#include "Breccia/Core/Log.h"

// BR_CORE_ASSERT / BR_ASSERT: programmer errors. Compiled out in Dist builds.
// BR_CORE_VERIFY / BR_VERIFY: always evaluated; break only when asserts are enabled.

#if !defined(BR_DIST)
	#define BR_ENABLE_ASSERTS 1
#endif

#define BR_INTERNAL_ASSERT_IMPL(logMacro, check, ...)                               \
	do                                                                              \
	{                                                                               \
		if (!(check))                                                               \
		{                                                                           \
			logMacro("Assertion '{}' failed at {}:{}", #check, __FILE__, __LINE__); \
			__VA_OPT__(logMacro(__VA_ARGS__);)                                      \
			BR_DEBUGBREAK();                                                        \
		}                                                                           \
	} while (0)

#define BR_INTERNAL_VERIFY_IMPL(logMacro, check, ...)                            \
	do                                                                           \
	{                                                                            \
		if (!(check))                                                            \
		{                                                                        \
			logMacro("Verify '{}' failed at {}:{}", #check, __FILE__, __LINE__); \
			__VA_OPT__(logMacro(__VA_ARGS__);)                                   \
			BR_INTERNAL_VERIFY_BREAK();                                          \
		}                                                                        \
	} while (0)

#if BR_ENABLE_ASSERTS
	#define BR_INTERNAL_VERIFY_BREAK() BR_DEBUGBREAK()
	#define BR_CORE_ASSERT(check, ...) BR_INTERNAL_ASSERT_IMPL(BR_CORE_CRITICAL, check __VA_OPT__(, ) __VA_ARGS__)
	#define BR_ASSERT(check, ...) BR_INTERNAL_ASSERT_IMPL(BR_CRITICAL, check __VA_OPT__(, ) __VA_ARGS__)
#else
	#define BR_INTERNAL_VERIFY_BREAK() ((void)0)
	#define BR_CORE_ASSERT(check, ...) ((void)0)
	#define BR_ASSERT(check, ...) ((void)0)
#endif

#define BR_CORE_VERIFY(check, ...) BR_INTERNAL_VERIFY_IMPL(BR_CORE_CRITICAL, check __VA_OPT__(, ) __VA_ARGS__)
#define BR_VERIFY(check, ...) BR_INTERNAL_VERIFY_IMPL(BR_CRITICAL, check __VA_OPT__(, ) __VA_ARGS__)
