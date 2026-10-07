# Fails if any Basalt source file is not formatted according to .clang-format.
# Usage: cmake -DCLANG_FORMAT=<path> -DSOURCE_DIR=<repo root> -P CheckFormat.cmake

file(GLOB_RECURSE files
	"${SOURCE_DIR}/Basalt/Source/*.cpp" "${SOURCE_DIR}/Basalt/Source/*.h"
	"${SOURCE_DIR}/Basalt-Editor/Source/*.cpp" "${SOURCE_DIR}/Basalt-Editor/Source/*.h"
	"${SOURCE_DIR}/Basalt-Runtime/Source/*.cpp" "${SOURCE_DIR}/Basalt-Runtime/Source/*.h"
	"${SOURCE_DIR}/Basalt-CLI/Source/*.cpp" "${SOURCE_DIR}/Basalt-CLI/Source/*.h"
	"${SOURCE_DIR}/Tests/Source/*.cpp" "${SOURCE_DIR}/Tests/Source/*.h"
	"${SOURCE_DIR}/Tests/Fuzz/*.cpp" "${SOURCE_DIR}/Tests/Fuzz/*.h")

execute_process(
	COMMAND "${CLANG_FORMAT}" --dry-run --Werror ${files}
	WORKING_DIRECTORY "${SOURCE_DIR}"
	RESULT_VARIABLE result
	ERROR_VARIABLE output)

if(NOT result EQUAL 0)
	message("${output}")
	message(FATAL_ERROR "Code is not formatted. Run scripts/format.sh (or clang-format -i) and commit the result.")
endif()
