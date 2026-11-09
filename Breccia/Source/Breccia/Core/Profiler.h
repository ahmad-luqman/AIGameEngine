#pragma once

// No Tracy dependency, data collection or argument evaluation in ordinary builds.
#if defined(BRECCIA_PROFILE) && !defined(BR_DIST)
	#include <tracy/Tracy.hpp>
	#define BR_PROFILE_SCOPE(name) ZoneScopedN(name)
	#define BR_PROFILE_FRAME() FrameMark
	#define BR_PROFILE_THREAD(name) tracy::SetThreadName(name)
#else
	#define BR_PROFILE_SCOPE(name) static_cast<void>(0)
	#define BR_PROFILE_FRAME() static_cast<void>(0)
	#define BR_PROFILE_THREAD(name) static_cast<void>(0)
#endif
