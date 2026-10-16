# Runs COMMAND (a ;-list) and fails unless it exits with EXPECTED and its output matches MATCH (when given).
# ctest's PASS_REGULAR_EXPRESSION ignores the exit code, which is what these tests are about.
#   cmake -DCOMMAND=<program;args...> -DEXPECTED=<code> [-DMATCH=<regex>] -P ExpectExitCode.cmake
execute_process(COMMAND ${COMMAND} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
message("${output}")
if(NOT result STREQUAL "${EXPECTED}")
	message(FATAL_ERROR "expected exit code ${EXPECTED}, got '${result}'")
endif()
if(DEFINED MATCH AND NOT output MATCHES "${MATCH}")
	message(FATAL_ERROR "output does not match '${MATCH}'")
endif()
