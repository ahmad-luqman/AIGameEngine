# Runs cmake/CheckFormat.cmake on generated source trees around its batch size (100 files):
# - clean trees of 100, 101 and 200 files (the last batch full, partial or absent) pass and report that many files;
# - a misformatted first file, 100th file (the end of a full batch), 101st file of 101 (a partial last batch) and 200th
#   file of 200 (the end of a full last batch) each fail, naming the file;
# - an empty tree and a clang-format that cannot start (whatever the length of its path) fail with their own messages.
# Usage: cmake -DCLANG_FORMAT=<path> -DREPO_DIR=<repo root> -DWORK_DIR=<scratch dir> -P CheckFormatBatches.cmake

set(problems "")

# File N's name, zero-padded (File001.cpp) so the glob's lexicographic order is the numeric one and file N lands in batch
# (N - 1) / 100.
function(file_name n outVar)
	math(EXPR padded "1000 + ${n}")
	string(SUBSTRING "${padded}" 1 3 index)
	set(${outVar} "File${index}.cpp" PARENT_SCOPE)
endfunction()

# Writes count files into a fresh tree; badIndex (1-based, 0 for none) names a misformatted one.
function(make_tree name count badIndex)
	set(root "${WORK_DIR}/${name}")
	file(REMOVE_RECURSE "${root}")
	file(MAKE_DIRECTORY "${root}/Basalt/Source")
	file(COPY "${REPO_DIR}/.clang-format" DESTINATION "${root}")
	if(count GREATER 0)
		foreach(i RANGE 1 ${count})
			set(content "int value${i};\n")
			if(i EQUAL badIndex)
				set(content "int   value${i} ;\n")
			endif()
			file_name(${i} fileName)
			file(WRITE "${root}/Basalt/Source/${fileName}" "${content}")
		endforeach()
	endif()
endfunction()

# Runs the check on a tree and records a problem when it passes or fails unexpectedly, or when its output does not match
# pattern. The output's whitespace is collapsed first: CMake wraps message() text at a width, so where a phrase breaks
# depends on the length of the paths before it.
function(expect name clangFormat shouldPass pattern)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" "-DCLANG_FORMAT=${clangFormat}" "-DSOURCE_DIR=${WORK_DIR}/${name}" -P "${REPO_DIR}/cmake/CheckFormat.cmake"
		RESULT_VARIABLE result
		OUTPUT_VARIABLE output
		ERROR_VARIABLE output)
	string(REGEX REPLACE "[ \t\r\n]+" " " output "${output}")
	if(shouldPass AND NOT result EQUAL 0)
		set(problems "${problems}\n${name}: expected to pass, got ${result}: ${output}" PARENT_SCOPE)
	elseif(NOT shouldPass AND result EQUAL 0)
		set(problems "${problems}\n${name}: expected to fail, but passed: ${output}" PARENT_SCOPE)
	elseif(NOT output MATCHES "${pattern}")
		set(problems "${problems}\n${name}: the output does not match '${pattern}': ${output}" PARENT_SCOPE)
	else()
		set(lastMatched TRUE PARENT_SCOPE)
		return()
	endif()
	set(lastMatched FALSE PARENT_SCOPE)
endfunction()

foreach(count 100 101 200)
	make_tree("Clean${count}" ${count} 0)
	expect("Clean${count}" "${CLANG_FORMAT}" TRUE "${count} files formatted as ")
endforeach()

# Name;file count;misformatted file.
foreach(case "BadFirst;101;1" "BadEndOfFullBatch;101;100" "BadInPartialLastBatch;101;101" "BadEndOfLastBatch;200;200")
	list(GET case 0 name)
	list(GET case 1 count)
	list(GET case 2 bad)
	make_tree(${name} ${count} ${bad})
	file_name(${bad} fileName)
	string(REPLACE "." "\\." filePattern "${fileName}")
	expect(${name} "${CLANG_FORMAT}" FALSE "${filePattern}")
endforeach()

make_tree(Empty 0 0)
expect(Empty "${CLANG_FORMAT}" FALSE "No sources found")

# The report names the path, so CMake's line wrapping moves with its length (on CI's work directory "failed to run" broke
# over two lines). Paths from the root of 33 to 73 characters move the break through the whole phrase (it falls inside
# it at 44-51 and 59-64 characters); from about 65 characters the path takes a line of its own.
set(pad "")
foreach(padLength RANGE 0 40)
	expect(Clean100 "/no-such-dir${pad}/no-such-clang-format" FALSE "failed to run")
	if(NOT lastMatched)
		string(APPEND problems " (${padLength}-character pad)")
		break()
	endif()
	string(APPEND pad "x")
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
if(problems)
	message(FATAL_ERROR "CheckFormat.cmake:${problems}")
endif()
