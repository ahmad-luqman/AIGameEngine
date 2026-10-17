// The translation unit that compiles miniaudio's reverb node (extras/nodes/ma_reverb_node: Freeverb through
// verblib, both in the pinned miniaudio source). Kept apart from MiniaudioImplementation.cpp so its defines and
// warnings stay out of miniaudio's own implementation.
#include <miniaudio.h>

#if defined(_MSC_VER)
	// Third-party C compiled inside a Basalt target: MSVC reports some code-generation warnings (such as C4701) even
	// for external headers, so silence them for this file only.
	#pragma warning(push, 0)
#endif
// The node ships as a .c file with no header-only mode; including it is how its example builds it.
#include <extras/nodes/ma_reverb_node/ma_reverb_node.c> // NOLINT(bugprone-suspicious-include)
#if defined(_MSC_VER)
	#pragma warning(pop)
#endif
