# Fails if any Basalt source file is not formatted according to .clang-format.
# Usage: cmake -DCLANG_FORMAT=<path> -DSOURCE_DIR=<repo root> -P CheckFormat.cmake
# The directories match scripts/format.sh's; change both together.

file(GLOB_RECURSE files RELATIVE "${SOURCE_DIR}"
	"${SOURCE_DIR}/Basalt/Source/*.cpp" "${SOURCE_DIR}/Basalt/Source/*.h"
	"${SOURCE_DIR}/Basalt-Editor/Source/*.cpp" "${SOURCE_DIR}/Basalt-Editor/Source/*.h"
	"${SOURCE_DIR}/Basalt-Runtime/Source/*.cpp" "${SOURCE_DIR}/Basalt-Runtime/Source/*.h"
	"${SOURCE_DIR}/Basalt-CLI/Source/*.cpp" "${SOURCE_DIR}/Basalt-CLI/Source/*.h"
	"${SOURCE_DIR}/Tests/Source/*.cpp" "${SOURCE_DIR}/Tests/Source/*.h"
	"${SOURCE_DIR}/Tests/Rendering/*.cpp" "${SOURCE_DIR}/Tests/Rendering/*.h"
	"${SOURCE_DIR}/Tests/Benchmarks/*.cpp" "${SOURCE_DIR}/Tests/Benchmarks/*.h"
	"${SOURCE_DIR}/Tests/Fuzz/*.cpp" "${SOURCE_DIR}/Tests/Fuzz/*.h")

# An empty glob (a wrong SOURCE_DIR, or sources moved away from the patterns) would otherwise check nothing and pass.
list(LENGTH files count)
if(count EQUAL 0)
	message(FATAL_ERROR "No sources found under '${SOURCE_DIR}'")
endif()

# Batches keep each command line well under Windows' 32767-character limit; one call with every file exceeds it
# and fails to start ("name too long") without ever reporting formatting.
set(unformatted FALSE)
set(batchSize 100)
set(begin 0)
while(begin LESS count)
	list(SUBLIST files ${begin} ${batchSize} batch)
	execute_process(
		COMMAND "${CLANG_FORMAT}" --dry-run --Werror ${batch}
		WORKING_DIRECTORY "${SOURCE_DIR}"
		RESULT_VARIABLE result
		ERROR_VARIABLE output)
	# A launch failure leaves a message, not an exit code, in result: report it as such, not as unformatted code.
	if(NOT result MATCHES "^[0-9]+$")
		message(FATAL_ERROR "clang-format ('${CLANG_FORMAT}') failed to run: ${result}")
	elseif(NOT result EQUAL 0)
		message("${output}")
		set(unformatted TRUE)
	endif()
	math(EXPR begin "${begin} + ${batchSize}")
endwhile()

if(unformatted)
	message(FATAL_ERROR "Code is not formatted. Run scripts/format.sh (or clang-format -i) and commit the result.")
endif()

# Evidence of what a passing run checked (Tests/Format/CheckFormatBatches.cmake reads the count).
execute_process(COMMAND "${CLANG_FORMAT}" --version OUTPUT_VARIABLE clangFormatVersion ERROR_QUIET
	OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT clangFormatVersion)
	set(clangFormatVersion "${CLANG_FORMAT}")
endif()
message(STATUS "${count} files formatted as ${clangFormatVersion} expects")
