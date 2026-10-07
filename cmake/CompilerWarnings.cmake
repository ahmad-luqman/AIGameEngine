# Strict warnings for Basalt's own targets. Third-party code is never compiled with these.

function(basalt_set_warnings target)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:preprocessor
			/wd4100 # unreferenced formal parameter (common in virtual overrides)
		)
	else()
		target_compile_options(${target} PRIVATE
			-Wall -Wextra -Wpedantic
			-Wshadow -Wnon-virtual-dtor -Wcast-align -Woverloaded-virtual
			-Wnull-dereference -Wimplicit-fallthrough
			-Wno-unused-parameter
			-Wno-missing-field-initializers
		)
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
