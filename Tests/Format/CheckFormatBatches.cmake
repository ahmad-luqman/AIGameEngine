# Runs cmake/CheckFormat.cmake on generated source trees around its batch size: clean trees must pass whatever their
# file count (the last batch may be full, partial or absent), and a misformatted file in the last batch, an empty
# tree and a clang-format that cannot start (whatever the length of its path) must each fail.
# Usage: cmake -DCLANG_FORMAT=<path> -DREPO_DIR=<repo root> -DWORK_DIR=<scratch dir> -P CheckFormatBatches.cmake

set(problems "")

# Writes count files into a fresh tree; badIndex (1-based, 0 for none) names a misformatted one. Names are zero-padded
# (File001.cpp) so the glob's lexicographic order is the numeric one and file N lands in batch (N - 1) / 100.
function(make_tree name count badIndex)
	set(root "${WORK_DIR}/${name}")
	file(REMOVE_RECURSE "${root}")
	file(MAKE_DIRECTORY "${root}/Basalt/Source")
	file(COPY "${REPO_DIR}/.clang-format" DESTINATION "${root}")
	foreach(i RANGE 1 ${count})
		if(count EQUAL 0)
			break()
		endif()
		math(EXPR padded "1000 + ${i}")
		string(SUBSTRING "${padded}" 1 3 index)
		if(i EQUAL badIndex)
			file(WRITE "${root}/Basalt/Source/File${index}.cpp" "int   value${i} ;\n")
		else()
			file(WRITE "${root}/Basalt/Source/File${index}.cpp" "int value${i};\n")
		endif()
	endforeach()
endfunction()

# Runs the check on a tree and records a problem when its outcome is not the expected one. lastOutput has its whitespace
# collapsed: CMake wraps message() text at a width, so where a phrase breaks depends on the length of the paths before it
# (on CI's runner path "failed to run" split over two lines).
function(expect name clangFormat shouldPass)
	# A failed expectation leaves it empty, so the follow-up checks never read an earlier case's output.
	set(lastOutput "" PARENT_SCOPE)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" "-DCLANG_FORMAT=${clangFormat}" "-DSOURCE_DIR=${WORK_DIR}/${name}" -P "${REPO_DIR}/cmake/CheckFormat.cmake"
		RESULT_VARIABLE result
		OUTPUT_VARIABLE output
		ERROR_VARIABLE output)
	if(shouldPass AND NOT result EQUAL 0)
		set(problems "${problems}\n${name}: expected to pass, got ${result}:\n${output}" PARENT_SCOPE)
	elseif(NOT shouldPass AND result EQUAL 0)
		set(problems "${problems}\n${name}: expected to fail, but passed" PARENT_SCOPE)
	else()
		string(REGEX REPLACE "[ \t\r\n]+" " " output "${output}")
		set(lastOutput "${output}" PARENT_SCOPE)
	endif()
endfunction()

foreach(count 100 101 200)
	make_tree("Clean${count}" ${count} 0)
	expect("Clean${count}" "${CLANG_FORMAT}" TRUE)
endforeach()

make_tree(BadInLastBatch 101 101)
expect(BadInLastBatch "${CLANG_FORMAT}" FALSE)
if(NOT lastOutput MATCHES "File101\\.cpp")
	set(problems "${problems}\nBadInLastBatch: the failure does not name File101.cpp:\n${lastOutput}")
endif()

make_tree(Empty 0 0)
expect(Empty "${CLANG_FORMAT}" FALSE)
if(NOT lastOutput MATCHES "No sources found")
	set(problems "${problems}\nEmpty: the failure does not say no sources were found:\n${lastOutput}")
endif()

# The report names the path, so CMake's line wrapping moves with its length (on CI's work directory "failed to run" broke
# over two lines). Paths from the root, 33 to 113 characters long, put every wrap position inside the phrase; a longer
# path only takes a line of its own.
set(pad "")
foreach(padLength RANGE 0 80)
	expect(Clean100 "/no-such-dir${pad}/no-such-clang-format" FALSE)
	if(NOT lastOutput MATCHES "failed to run")
		set(problems "${problems}\nMissing clang-format (${padLength}-character pad): reported as something other than a launch failure:\n${lastOutput}")
		break()
	endif()
	string(APPEND pad "x")
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
if(problems)
	message(FATAL_ERROR "CheckFormat.cmake:${problems}")
endif()
