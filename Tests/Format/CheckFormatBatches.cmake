# Runs cmake/CheckFormat.cmake on generated source trees around its batch size: clean trees must pass whatever their
# file count (the last batch may be full, partial or absent), and a misformatted file in the last batch, an empty
# tree and a clang-format that cannot start must each fail.
# Usage: cmake -DCLANG_FORMAT=<path> -DREPO_DIR=<repo root> -DWORK_DIR=<scratch dir> -P CheckFormatBatches.cmake

set(problems "")

# Writes count files into a fresh tree; badIndex (1-based, 0 for none) names a misformatted one.
function(make_tree name count badIndex)
	set(root "${WORK_DIR}/${name}")
	file(REMOVE_RECURSE "${root}")
	file(MAKE_DIRECTORY "${root}/Basalt/Source")
	file(COPY "${REPO_DIR}/.clang-format" DESTINATION "${root}")
	foreach(i RANGE 1 ${count})
		if(count EQUAL 0)
			break()
		endif()
		if(i EQUAL badIndex)
			file(WRITE "${root}/Basalt/Source/File${i}.cpp" "int   value${i} ;\n")
		else()
			file(WRITE "${root}/Basalt/Source/File${i}.cpp" "int value${i};\n")
		endif()
	endforeach()
endfunction()

# Runs the check on a tree and records a problem when its outcome is not the expected one.
function(expect name clangFormat shouldPass)
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
		set(lastOutput "${output}" PARENT_SCOPE)
	endif()
endfunction()

foreach(count 100 101 200)
	make_tree("Clean${count}" ${count} 0)
	expect("Clean${count}" "${CLANG_FORMAT}" TRUE)
endforeach()

make_tree(BadInLastBatch 101 101)
expect(BadInLastBatch "${CLANG_FORMAT}" FALSE)
if(NOT lastOutput MATCHES "File101.cpp")
	set(problems "${problems}\nBadInLastBatch: the failure does not name File101.cpp:\n${lastOutput}")
endif()

make_tree(Empty 0 0)
expect(Empty "${CLANG_FORMAT}" FALSE)
if(NOT lastOutput MATCHES "No sources found")
	set(problems "${problems}\nEmpty: the failure does not say no sources were found:\n${lastOutput}")
endif()

expect(Clean100 "${WORK_DIR}/no-such-clang-format" FALSE)
if(NOT lastOutput MATCHES "failed to run")
	set(problems "${problems}\nMissing clang-format: reported as something other than a launch failure:\n${lastOutput}")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
if(problems)
	message(FATAL_ERROR "CheckFormat.cmake:${problems}")
endif()
