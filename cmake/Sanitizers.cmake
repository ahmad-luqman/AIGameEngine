# Sanitizer builds (ASan/UBSan/...). Included before the dependencies so that every static library,
# third-party code included, is instrumented: mixing instrumented and plain code gives false reports.

set(BRECCIA_SANITIZE "" CACHE STRING "Semicolon-separated sanitizers to enable, e.g. \"address;undefined\" (Clang/GCC)")

if(BRECCIA_SANITIZE)
	if(MSVC)
		message(FATAL_ERROR "BRECCIA_SANITIZE is only supported with Clang and GCC")
	endif()

	list(JOIN BRECCIA_SANITIZE "," brecciaSanitizers)
	# UB must fail the test that triggers it instead of printing a report and carrying on.
	set(brecciaSanitizeFlags -fsanitize=${brecciaSanitizers} -fno-omit-frame-pointer)
	if("undefined" IN_LIST BRECCIA_SANITIZE)
		list(APPEND brecciaSanitizeFlags -fno-sanitize-recover=undefined)
	endif()
	add_compile_options(${brecciaSanitizeFlags})
	add_link_options(${brecciaSanitizeFlags})
	message(STATUS "Breccia: sanitizers enabled: ${brecciaSanitizers}")
endif()

# Fuzzing (Tests/Fuzz): every target gets libFuzzer's coverage instrumentation, and the fuzz executables
# link libFuzzer itself. Combine with BRECCIA_SANITIZE="address;undefined" so bugs crash immediately.
option(BRECCIA_FUZZ "Build the libFuzzer targets in Tests/Fuzz (Clang only)" OFF)
if(BRECCIA_FUZZ)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		message(FATAL_ERROR "BRECCIA_FUZZ needs Clang (libFuzzer); on macOS use Homebrew LLVM's clang")
	endif()
	add_compile_options(-fsanitize=fuzzer-no-link)
	message(STATUS "Breccia: fuzz targets enabled")
endif()

# Environment for ctest so sanitizer runs behave the same locally and on CI. Suppression files hold
# known third-party reports only; Breccia code is fixed, never suppressed.
set(brecciaDetectLeaks 1)
if(APPLE)
	# LeakSanitizer is not supported by Apple's ASan runtime and aborts when requested.
	set(brecciaDetectLeaks 0)
endif()
set(BRECCIA_SANITIZER_ENVIRONMENT
	"ASAN_OPTIONS=detect_leaks=${brecciaDetectLeaks}:strict_string_checks=1:check_initialization_order=1"
	"LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_LIST_DIR}/sanitizers/lsan.supp:print_suppressions=0"
	"UBSAN_OPTIONS=print_stacktrace=1:suppressions=${CMAKE_CURRENT_LIST_DIR}/sanitizers/ubsan.supp")
