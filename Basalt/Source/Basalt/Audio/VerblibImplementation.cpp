// The translation unit that compiles verblib (Freeverb), the reverb in miniaudio's pinned source
// (extras/nodes/ma_reverb_node). ReverbNode wraps it in a node of its own.
#if defined(_MSC_VER)
	// Third-party C compiled inside a Basalt target: MSVC reports some code-generation warnings (such as C4701) even
	// for external headers, so silence them for this file only.
	#pragma warning(push, 0)
#endif
#define VERBLIB_IMPLEMENTATION
#include <extras/nodes/ma_reverb_node/verblib.h>
#if defined(_MSC_VER)
	#pragma warning(pop)
#endif
