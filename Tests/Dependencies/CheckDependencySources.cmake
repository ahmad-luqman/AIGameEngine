# Configures a small project that uses cmake/DependencySources.cmake against generated source directories: a directory
# stamped for the pins supplies each <name>-src directory and nothing else; a wrong, missing or absent stamp supplies
# nothing; and a reconfigure without BASALT_DEPS_SOURCE_DIR forgets the paths an earlier configure cached (as
# FetchContent does). Also checks `scripts/ci-cache.sh check` where bash is available.
# Usage: cmake -DREPO_DIR=<repo root> -DWORK_DIR=<scratch dir> -P CheckDependencySources.cmake

set(problems "")
file(REMOVE_RECURSE "${WORK_DIR}")
set(project "${WORK_DIR}/Project")
set(pins "${project}/Pins.cmake")

file(WRITE "${pins}" "# pinned versions\n")
file(WRITE "${project}/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.25)
project(DependencySourcesCheck LANGUAGES NONE)
list(APPEND CMAKE_MODULE_PATH \"${REPO_DIR}/cmake\")
include(DependencySources)
basalt_use_dependency_sources(\"${pins}\")
foreach(name GLM VULKANHEADERS FILE QUX)
	# What FetchContent_MakeAvailable does with each override (FetchContent.cmake).
	set(FETCHCONTENT_SOURCE_DIR_\${name} \"\${FETCHCONTENT_SOURCE_DIR_\${name}}\" CACHE PATH \"\")
	message(STATUS \"override \${name}=\${FETCHCONTENT_SOURCE_DIR_\${name}}\")
endforeach()
")

# A source directory like the one `scripts/ci-cache.sh fetch-deps` fills, with a plain file and a build directory that must not count.
function(make_sources name stamp)
	set(dir "${WORK_DIR}/${name}")
	file(MAKE_DIRECTORY "${dir}/glm-src" "${dir}/vulkanheaders-src" "${dir}/qux-build")
	file(WRITE "${dir}/file-src" "")
	if(stamp STREQUAL "pins")
		# The same command scripts/ci-cache.sh stamps with.
		execute_process(COMMAND "${CMAKE_COMMAND}" -E sha256sum "${pins}" OUTPUT_VARIABLE line)
		string(REGEX MATCH "^[0-9a-f]+" stamp "${line}")
	endif()
	if(NOT stamp STREQUAL "none")
		file(WRITE "${dir}/pins.sha256" "${stamp}\n")
	endif()
endfunction()

# Configures the project in tree (env: the source directory or "unset") and keeps its output in lastOutput.
function(configure tree env)
	if(env STREQUAL "unset")
		set(command "${CMAKE_COMMAND}" -E env --unset=BASALT_DEPS_SOURCE_DIR)
	else()
		set(command "${CMAKE_COMMAND}" -E env "BASALT_DEPS_SOURCE_DIR=${env}")
	endif()
	execute_process(COMMAND ${command} "${CMAKE_COMMAND}" -S "${project}" -B "${WORK_DIR}/${tree}"
		RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
	if(NOT result EQUAL 0)
		set(problems "${problems}\n${tree}: configure failed:\n${output}" PARENT_SCOPE)
	endif()
	set(lastOutput "${output}" PARENT_SCOPE)
endfunction()

# Records a problem unless the last configure overrode exactly the given names (with paths in sourceDir).
function(expect_overrides label sourceDir)
	foreach(name GLM VULKANHEADERS FILE QUX)
		string(TOLOWER "${name}" lower)
		if(name IN_LIST ARGN)
			set(wanted "${sourceDir}/${lower}-src")
		else()
			set(wanted "")
		endif()
		string(REGEX MATCH "override ${name}=[^\n]*" line "${lastOutput}")
		if(NOT line STREQUAL "override ${name}=${wanted}")
			set(problems "${problems}\n${label}: expected 'override ${name}=${wanted}', got '${line}'")
		endif()
	endforeach()
	set(problems "${problems}" PARENT_SCOPE)
endfunction()

make_sources(Stamped pins)
configure(Tree "${WORK_DIR}/Stamped")
expect_overrides(Stamped "${WORK_DIR}/Stamped" GLM VULKANHEADERS)
if(NOT lastOutput MATCHES "2 dependency sources from")
	set(problems "${problems}\nStamped: the status does not count two sources:\n${lastOutput}")
endif()

# The cached paths from the first configure must not outlive the variable.
configure(Tree unset)
expect_overrides(Reconfigured "")

make_sources(WrongStamp 0123456789abcdef)
configure(WrongStampTree "${WORK_DIR}/WrongStamp")
expect_overrides(WrongStamp "")
if(NOT lastOutput MATCHES "ignoring BASALT_DEPS_SOURCE_DIR")
	set(problems "${problems}\nWrongStamp: no warning about the ignored directory:\n${lastOutput}")
endif()

make_sources(NoStamp none)
configure(NoStampTree "${WORK_DIR}/NoStamp")
expect_overrides(NoStamp "")
if(NOT lastOutput MATCHES "ignoring BASALT_DEPS_SOURCE_DIR")
	set(problems "${problems}\nNoStamp: no warning about the ignored directory:\n${lastOutput}")
endif()

# A CI cache miss: the directory does not exist yet, which is not worth a warning.
configure(MissingTree "${WORK_DIR}/Missing")
expect_overrides(Missing "")
if(lastOutput MATCHES "Warning")
	set(problems "${problems}\nMissing: unexpected warning:\n${lastOutput}")
endif()

# scripts/ci-cache.sh check: a tree that downloaded a source fails and names it; one that downloaded none passes; and
# with a launcher in the environment, a tree configured without it fails.
find_program(bash bash)
if(bash AND NOT WIN32)
	file(MAKE_DIRECTORY "${WORK_DIR}/Downloaded/_deps/glm-src" "${WORK_DIR}/Used/_deps/glm-build")
	file(MAKE_DIRECTORY "${WORK_DIR}/UsedCrlf/_deps")
	# Real caches are tens of kilobytes, more than a pipe buffer holds, with the launcher well before the end.
	string(REPEAT "SOME_ENTRY:STRING=value\n" 5000 padding)
	file(WRITE "${WORK_DIR}/Used/CMakeCache.txt" "CMAKE_CXX_COMPILER_LAUNCHER:STRING=ccache\n${padding}")
	# As CMake writes it on Windows.
	file(WRITE "${WORK_DIR}/UsedCrlf/CMakeCache.txt" "CMAKE_CXX_COMPILER_LAUNCHER:STRING=sccache\r\n")

	# Runs the check on tree with CMAKE_CXX_COMPILER_LAUNCHER set to launcher ("unset" for none).
	function(check tree launcher expectPass pattern)
		if(launcher STREQUAL "unset")
			set(env --unset=CMAKE_CXX_COMPILER_LAUNCHER)
		else()
			set(env "CMAKE_CXX_COMPILER_LAUNCHER=${launcher}")
		endif()
		execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${env} "${bash}" "${REPO_DIR}/scripts/ci-cache.sh" check "${WORK_DIR}/${tree}"
			RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
		if(expectPass AND NOT result EQUAL 0)
			set(problems "${problems}\nci-cache.sh check ${tree} (${launcher}): expected to pass, got ${result}:\n${output}" PARENT_SCOPE)
		elseif(NOT expectPass AND result EQUAL 0)
			set(problems "${problems}\nci-cache.sh check ${tree} (${launcher}): expected to fail, but passed:\n${output}" PARENT_SCOPE)
		elseif(NOT output MATCHES "${pattern}")
			set(problems "${problems}\nci-cache.sh check ${tree} (${launcher}): output does not match '${pattern}':\n${output}" PARENT_SCOPE)
		endif()
	endfunction()

	check(Downloaded unset FALSE "downloaded glm-src")
	check(Used unset TRUE "downloaded no dependency sources")
	check(Used ccache TRUE "compiles through ccache")
	check(Used sccache FALSE "does not compile through sccache")
	check(UsedCrlf sccache TRUE "compiles through sccache")

	execute_process(COMMAND "${bash}" "${REPO_DIR}/scripts/ci-cache.sh" check RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
	if(result EQUAL 0)
		set(problems "${problems}\nci-cache.sh: a missing argument was accepted")
	endif()
endif()

if(problems)
	message(FATAL_ERROR "Dependency source checks failed:${problems}")
endif()
message(STATUS "Dependency source checks passed")
