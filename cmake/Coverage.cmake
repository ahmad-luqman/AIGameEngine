# Source-based code coverage (Clang). Reports come from scripts/coverage.sh; third-party code is
# instrumented too but filtered out of the report.

option(BASALT_COVERAGE "Instrument the build for llvm-cov source coverage (Clang only)" OFF)

if(BASALT_COVERAGE)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		message(FATAL_ERROR "BASALT_COVERAGE needs Clang (source-based coverage)")
	endif()
	add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
	add_link_options(-fprofile-instr-generate)
	message(STATUS "Basalt: coverage instrumentation enabled")
endif()
