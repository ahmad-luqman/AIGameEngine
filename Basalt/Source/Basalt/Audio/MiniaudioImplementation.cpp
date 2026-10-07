// The single translation unit that compiles miniaudio's implementation.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#if defined(_MSC_VER)
	// Third-party code compiled inside a Basalt target: C4701 is a false positive in the WAV metadata
	// reader (the variable is only read when a flag set alongside it is true).
	#pragma warning(push)
	#pragma warning(disable : 4701)
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
	#pragma warning(pop)
#endif
