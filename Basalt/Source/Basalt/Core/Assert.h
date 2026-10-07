#pragma once

#include "Basalt/Core/Base.h"
#include "Basalt/Core/Log.h"

// BS_CORE_ASSERT / BS_ASSERT: programmer errors. Compiled out in Dist builds.
// BS_CORE_VERIFY / BS_VERIFY: always evaluated; break only when asserts are enabled.

#if !defined(BS_DIST)
	#define BS_ENABLE_ASSERTS 1
#endif

#define BS_INTERNAL_ASSERT_IMPL(logMacro, check, ...)                               \
	do                                                                              \
	{                                                                               \
		if (!(check))                                                               \
		{                                                                           \
			logMacro("Assertion '{}' failed at {}:{}", #check, __FILE__, __LINE__); \
			__VA_OPT__(logMacro(__VA_ARGS__);)                                      \
			BS_DEBUGBREAK();                                                        \
		}                                                                           \
	} while (0)

#define BS_INTERNAL_VERIFY_IMPL(logMacro, check, ...)                            \
	do                                                                           \
	{                                                                            \
		if (!(check))                                                            \
		{                                                                        \
			logMacro("Verify '{}' failed at {}:{}", #check, __FILE__, __LINE__); \
			__VA_OPT__(logMacro(__VA_ARGS__);)                                   \
			BS_INTERNAL_VERIFY_BREAK();                                          \
		}                                                                        \
	} while (0)

#if BS_ENABLE_ASSERTS
	#define BS_INTERNAL_VERIFY_BREAK() BS_DEBUGBREAK()
	#define BS_CORE_ASSERT(check, ...) BS_INTERNAL_ASSERT_IMPL(BS_CORE_CRITICAL, check __VA_OPT__(, ) __VA_ARGS__)
	#define BS_ASSERT(check, ...) BS_INTERNAL_ASSERT_IMPL(BS_CRITICAL, check __VA_OPT__(, ) __VA_ARGS__)
#else
	#define BS_INTERNAL_VERIFY_BREAK() ((void)0)
	#define BS_CORE_ASSERT(check, ...) ((void)0)
	#define BS_ASSERT(check, ...) ((void)0)
#endif

#define BS_CORE_VERIFY(check, ...) BS_INTERNAL_VERIFY_IMPL(BS_CORE_CRITICAL, check __VA_OPT__(, ) __VA_ARGS__)
#define BS_VERIFY(check, ...) BS_INTERNAL_VERIFY_IMPL(BS_CRITICAL, check __VA_OPT__(, ) __VA_ARGS__)
