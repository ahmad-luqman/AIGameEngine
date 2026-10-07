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
endfunction()

option(BASALT_WARNINGS_AS_ERRORS "Treat warnings in Basalt code as errors" ON)
