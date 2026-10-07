# Strict warnings for Basalt's own targets. Third-party code is never compiled with these.

function(basalt_set_warnings target)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:preprocessor /Zc:__cplusplus /bigobj
			/wd4100 # unreferenced formal parameter (common in virtual overrides)
		)
	else()
		target_compile_options(${target} PRIVATE
			-Wall -Wextra -Wpedantic
			-Wshadow -Wnon-virtual-dtor -Wcast-align -Woverloaded-virtual
			-Wimplicit-fallthrough
			-Wno-unused-parameter
			-Wno-missing-field-initializers
		)
		# GCC's -Wnull-dereference reports false positives in inlined sol2/EnTT code; clang's is precise.
		if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
			target_compile_options(${target} PRIVATE -Wnull-dereference)
		endif()
	endif()

	if(BASALT_WARNINGS_AS_ERRORS)
		if(MSVC)
			target_compile_options(${target} PRIVATE /WX)
		else()
			target_compile_options(${target} PRIVATE -Werror)
		endif()
	endif()

	# Static analysis runs on the same targets as the warnings, so third-party code is never checked.
	if(BASALT_CLANG_TIDY)
		set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${BASALT_CLANG_TIDY_COMMAND}")
	endif()
endfunction()

option(BASALT_WARNINGS_AS_ERRORS "Treat warnings in Basalt code as errors" ON)

# Checks are configured in the repository's .clang-tidy file.
option(BASALT_CLANG_TIDY "Run clang-tidy on Basalt's own targets while compiling" OFF)
if(BASALT_CLANG_TIDY)
	find_program(BASALT_CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-18 clang-tidy-19 HINTS /opt/homebrew/opt/llvm/bin)
	if(NOT BASALT_CLANG_TIDY_EXE)
		message(FATAL_ERROR "BASALT_CLANG_TIDY is ON but clang-tidy was not found")
	endif()
	set(BASALT_CLANG_TIDY_COMMAND "${BASALT_CLANG_TIDY_EXE}")
	# A non-Apple clang-tidy (Homebrew LLVM) does not know where the macOS SDK headers are.
	if(APPLE AND CMAKE_OSX_SYSROOT)
		list(APPEND BASALT_CLANG_TIDY_COMMAND "--extra-arg=-isysroot${CMAKE_OSX_SYSROOT}")
	endif()
endif()
