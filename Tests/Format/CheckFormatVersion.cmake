# Runs cmake/CheckClangFormatVersion.cmake against generated workflow files: the pin must be read whether it is quoted,
# commented or checked out with CRLF; a different major version, a missing pin or workflow, and a clang-format whose
# version cannot be told must warn, and fail with STRICT=ON.
# Usage: cmake -DCLANG_FORMAT=<path> -DREPO_DIR=<repo root> -DWORK_DIR=<scratch dir> -P CheckFormatVersion.cmake

set(problems "")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

execute_process(COMMAND "${CLANG_FORMAT}" --version OUTPUT_VARIABLE versionOutput OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT versionOutput MATCHES "clang-format version ([0-9]+)")
	message(FATAL_ERROR "Cannot tell the version of ${CLANG_FORMAT}: '${versionOutput}'")
endif()
set(major "${CMAKE_MATCH_1}")

# Runs the check on a workflow file. expected is the start of the problem it must report, or "" for none; a reported
# problem must warn (exit 0) without STRICT and fail with it.
function(expect name workflow clangFormat expected)
	foreach(strict OFF ON)
		execute_process(
			COMMAND "${CMAKE_COMMAND}" "-DCLANG_FORMAT=${clangFormat}" "-DCI_WORKFLOW=${workflow}" "-DSTRICT=${strict}"
				-P "${REPO_DIR}/cmake/CheckClangFormatVersion.cmake"
			RESULT_VARIABLE result
			OUTPUT_VARIABLE output
			ERROR_VARIABLE output)
		# CMake wraps message() text at a width that depends on the paths in it.
		string(REGEX REPLACE "[ \t\r\n]+" " " output "${output}")
		if(expected STREQUAL "")
			if(NOT result EQUAL 0 OR output MATCHES "Warning|Error")
				set(problems "${problems}\n${name} (STRICT=${strict}): expected no report, got ${result}: ${output}")
			endif()
		elseif(NOT output MATCHES "${expected}")
			set(problems "${problems}\n${name} (STRICT=${strict}): expected '${expected}', got: ${output}")
		elseif(strict AND result EQUAL 0)
			set(problems "${problems}\n${name}: STRICT=ON reported the problem but passed")
		elseif(NOT strict AND NOT result EQUAL 0)
			set(problems "${problems}\n${name}: a warning failed (${result}) without STRICT")
		endif()
	endforeach()
	set(problems "${problems}" PARENT_SCOPE)
endfunction()

# Writes a workflow whose format job pins version, as the line value (after "CLANG_FORMAT_VERSION:") is written.
function(make_workflow name value)
	file(WRITE "${WORK_DIR}/${name}.yml" "jobs:\n  format:\n    env:\n      CLANG_FORMAT_VERSION:${value}\n")
endfunction()

make_workflow(Plain " ${major}.1.3")
expect(Plain "${WORK_DIR}/Plain.yml" "${CLANG_FORMAT}" "")
make_workflow(DoubleQuoted " \"${major}.1.3\"")
expect(DoubleQuoted "${WORK_DIR}/DoubleQuoted.yml" "${CLANG_FORMAT}" "")
make_workflow(SingleQuoted " '${major}.1'")
expect(SingleQuoted "${WORK_DIR}/SingleQuoted.yml" "${CLANG_FORMAT}" "")
make_workflow(Commented "\t${major}.1.3  # bump with a reformat")
expect(Commented "${WORK_DIR}/Commented.yml" "${CLANG_FORMAT}" "")
file(WRITE "${WORK_DIR}/Crlf.yml" "jobs:\r\n  format:\r\n    env:\r\n      CLANG_FORMAT_VERSION: ${major}.1.3\r\n")
expect(Crlf "${WORK_DIR}/Crlf.yml" "${CLANG_FORMAT}" "")

math(EXPR otherMajor "${major} + 1")
make_workflow(OtherMajor " ${otherMajor}.1.0")
expect(OtherMajor "${WORK_DIR}/OtherMajor.yml" "${CLANG_FORMAT}" "but CI checks with ${otherMajor}\\.1\\.0")
make_workflow(NotAVersion " latest")
expect(NotAVersion "${WORK_DIR}/NotAVersion.yml" "${CLANG_FORMAT}" "No CLANG_FORMAT_VERSION found")
file(WRITE "${WORK_DIR}/NoPin.yml" "jobs:\n  format:\n    runs-on: ubuntu-24.04\n")
expect(NoPin "${WORK_DIR}/NoPin.yml" "${CLANG_FORMAT}" "No CLANG_FORMAT_VERSION found")
expect(NoWorkflow "${WORK_DIR}/missing.yml" "${CLANG_FORMAT}" "No CLANG_FORMAT_VERSION found")
expect(NoClangFormat "${WORK_DIR}/Plain.yml" "${WORK_DIR}/no-such-clang-format" "Cannot tell the version")

file(REMOVE_RECURSE "${WORK_DIR}")
if(problems)
	message(FATAL_ERROR "CheckClangFormatVersion.cmake:${problems}")
endif()
