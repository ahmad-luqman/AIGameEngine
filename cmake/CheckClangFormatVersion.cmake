# Compares CLANG_FORMAT's major version with CI's pin (CLANG_FORMAT_VERSION in .github/workflows/ci.yml): another major
# version formats some code differently. Warns by default; STRICT=ON (CI's format job) fails instead, so a pin line the
# parser no longer understands is caught there. Included by Tests/CMakeLists.txt (configure), run by scripts/format.sh
# and CI as: cmake -DCLANG_FORMAT=<path> [-DSTRICT=ON] [-DCI_WORKFLOW=<file>] -P CheckClangFormatVersion.cmake
# CI_WORKFLOW (default: this repository's ci.yml) lets Tests/Format/CheckFormatVersion.cmake feed it variants.

if(NOT DEFINED CI_WORKFLOW)
	set(CI_WORKFLOW "${CMAKE_CURRENT_LIST_DIR}/../.github/workflows/ci.yml")
endif()

if(STRICT)
	set(clangFormatVersionMode FATAL_ERROR)
else()
	set(clangFormatVersionMode WARNING)
endif()

# Accepts the value quoted or not, followed by a comment, and a CRLF checkout (Windows).
set(pinnedFormatVersion "")
if(EXISTS "${CI_WORKFLOW}")
	file(STRINGS "${CI_WORKFLOW}" pinnedFormatLines REGEX "^[ \t]*CLANG_FORMAT_VERSION:")
	foreach(line IN LISTS pinnedFormatLines)
		if(line MATCHES "^[ \t]*CLANG_FORMAT_VERSION:[ \t]*[\"']?([0-9]+(\\.[0-9]+)*)[\"']?[ \t\r]*(#.*)?$")
			set(pinnedFormatVersion "${CMAKE_MATCH_1}")
			break()
		endif()
	endforeach()
endif()

execute_process(COMMAND "${CLANG_FORMAT}" --version RESULT_VARIABLE localFormatResult OUTPUT_VARIABLE localFormatOutput
	ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
set(localFormatVersion "")
if(localFormatResult EQUAL 0 AND localFormatOutput MATCHES "clang-format version ([0-9]+(\\.[0-9]+)*)")
	set(localFormatVersion "${CMAKE_MATCH_1}")
endif()

string(REGEX MATCH "^[0-9]+" pinnedFormatMajor "${pinnedFormatVersion}")
string(REGEX MATCH "^[0-9]+" localFormatMajor "${localFormatVersion}")
if(NOT pinnedFormatVersion)
	message(${clangFormatVersionMode} "No CLANG_FORMAT_VERSION found in ${CI_WORKFLOW}, so ${CLANG_FORMAT}'s version "
		"is not compared with CI's.")
elseif(NOT localFormatVersion)
	message(${clangFormatVersionMode} "Cannot tell the version of ${CLANG_FORMAT} ('${localFormatOutput}'); CI checks "
		"with clang-format ${pinnedFormatVersion}.")
elseif(NOT localFormatMajor STREQUAL pinnedFormatMajor)
	message(${clangFormatVersionMode} "${CLANG_FORMAT} is clang-format ${localFormatVersion}, but CI checks with "
		"${pinnedFormatVersion}, which formats some code differently. Install that one (python3 -m venv <dir> && "
		"<dir>/bin/pip install clang-format==${pinnedFormatVersion}) and set CLANG_FORMAT=<dir>/bin/clang-format.")
endif()
