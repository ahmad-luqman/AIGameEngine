# Sanitizer builds (ASan/UBSan/...). Included before the dependencies so that every static library,
# third-party code included, is instrumented: mixing instrumented and plain code gives false reports.

set(BASALT_SANITIZE "" CACHE STRING "Semicolon-separated sanitizers to enable, e.g. \"address;undefined\" (Clang/GCC)")

if(BASALT_SANITIZE)
	if(MSVC)
		message(FATAL_ERROR "BASALT_SANITIZE is only supported with Clang and GCC")
	endif()

	list(JOIN BASALT_SANITIZE "," basaltSanitizers)
	# UB must fail the test that triggers it instead of printing a report and carrying on.
	set(basaltSanitizeFlags -fsanitize=${basaltSanitizers} -fno-omit-frame-pointer)
	if("undefined" IN_LIST BASALT_SANITIZE)
		list(APPEND basaltSanitizeFlags -fno-sanitize-recover=undefined)
	endif()
	add_compile_options(${basaltSanitizeFlags})
	add_link_options(${basaltSanitizeFlags})
	message(STATUS "Basalt: sanitizers enabled: ${basaltSanitizers}")
endif()

# Fuzzing (Tests/Fuzz): every target gets libFuzzer's coverage instrumentation, and the fuzz executables
# link libFuzzer itself. Combine with BASALT_SANITIZE="address;undefined" so bugs crash immediately.
option(BASALT_FUZZ "Build the libFuzzer targets in Tests/Fuzz (Clang only)" OFF)
if(BASALT_FUZZ)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		message(FATAL_ERROR "BASALT_FUZZ needs Clang (libFuzzer); on macOS use Homebrew LLVM's clang")
	endif()
	add_compile_options(-fsanitize=fuzzer-no-link)
	message(STATUS "Basalt: fuzz targets enabled")
endif()

# Environment for ctest so sanitizer runs behave the same locally and on CI. Suppression files hold
# known third-party reports only; Basalt code is fixed, never suppressed.
set(basaltDetectLeaks 1)
if(APPLE)
	# LeakSanitizer is not supported by Apple's ASan runtime and aborts when requested.
	set(basaltDetectLeaks 0)
endif()
set(BASALT_SANITIZER_ENVIRONMENT
	"ASAN_OPTIONS=detect_leaks=${basaltDetectLeaks}:strict_string_checks=1:check_initialization_order=1"
	"LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_LIST_DIR}/sanitizers/lsan.supp:print_suppressions=0"
	"UBSAN_OPTIONS=print_stacktrace=1:suppressions=${CMAKE_CURRENT_LIST_DIR}/sanitizers/ubsan.supp")
