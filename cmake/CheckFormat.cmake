# Fails if any Basalt source file is not formatted according to .clang-format.
# Usage: cmake -DCLANG_FORMAT=<path> -DSOURCE_DIR=<repo root> -P CheckFormat.cmake

file(GLOB_RECURSE files RELATIVE "${SOURCE_DIR}"
	"${SOURCE_DIR}/Basalt/Source/*.cpp" "${SOURCE_DIR}/Basalt/Source/*.h"
	"${SOURCE_DIR}/Basalt-Editor/Source/*.cpp" "${SOURCE_DIR}/Basalt-Editor/Source/*.h"
	"${SOURCE_DIR}/Basalt-Runtime/Source/*.cpp" "${SOURCE_DIR}/Basalt-Runtime/Source/*.h"
	"${SOURCE_DIR}/Basalt-CLI/Source/*.cpp" "${SOURCE_DIR}/Basalt-CLI/Source/*.h"
	"${SOURCE_DIR}/Tests/Source/*.cpp" "${SOURCE_DIR}/Tests/Source/*.h"
	"${SOURCE_DIR}/Tests/Rendering/*.cpp" "${SOURCE_DIR}/Tests/Rendering/*.h"
	"${SOURCE_DIR}/Tests/Benchmarks/*.cpp" "${SOURCE_DIR}/Tests/Benchmarks/*.h"
	"${SOURCE_DIR}/Tests/Fuzz/*.cpp" "${SOURCE_DIR}/Tests/Fuzz/*.h")

# Batches keep each command line well under Windows' 32767-character limit; one call with every file exceeds it
# and fails to start ("name too long") without ever reporting formatting.
set(failed FALSE)
list(LENGTH files count)
set(batchSize 100)
foreach(begin RANGE 0 ${count} ${batchSize})
	list(SUBLIST files ${begin} ${batchSize} batch)
	if(NOT batch)
		continue()
	endif()
	execute_process(
		COMMAND "${CLANG_FORMAT}" --dry-run --Werror ${batch}
		WORKING_DIRECTORY "${SOURCE_DIR}"
		RESULT_VARIABLE result
		ERROR_VARIABLE output)
	if(NOT result EQUAL 0)
		message("${result}\n${output}")
		set(failed TRUE)
	endif()
endforeach()

if(failed)
	message(FATAL_ERROR "Code is not formatted. Run scripts/format.sh (or clang-format -i) and commit the result.")
endif()
