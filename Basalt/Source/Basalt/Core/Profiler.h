#pragma once

// No Tracy dependency, data collection or argument evaluation in ordinary builds.
#if defined(BASALT_PROFILE) && !defined(BS_DIST)
	#include <tracy/Tracy.hpp>
	#define BS_PROFILE_SCOPE(name) ZoneScopedN(name)
	#define BS_PROFILE_FRAME() FrameMark
	#define BS_PROFILE_THREAD(name) tracy::SetThreadName(name)
#else
	#define BS_PROFILE_SCOPE(name) static_cast<void>(0)
	#define BS_PROFILE_FRAME() static_cast<void>(0)
	#define BS_PROFILE_THREAD(name) static_cast<void>(0)
#endif
