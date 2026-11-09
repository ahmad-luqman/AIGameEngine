# Source-based code coverage (Clang). Reports come from scripts/coverage.sh; third-party code is
# instrumented too but filtered out of the report.

option(BRECCIA_COVERAGE "Instrument the build for llvm-cov source coverage (Clang only)" OFF)

if(BRECCIA_COVERAGE)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		message(FATAL_ERROR "BRECCIA_COVERAGE needs Clang (source-based coverage)")
	endif()
	add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
	add_link_options(-fprofile-instr-generate)
	message(STATUS "Breccia: coverage instrumentation enabled")
endif()
