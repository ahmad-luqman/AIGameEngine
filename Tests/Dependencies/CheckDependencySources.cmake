# Checks CI's dependency-source cache without network access:
# - cmake/DependencySources.cmake, through a small project: a directory stamped for the pins supplies each <name>-src
#   directory and nothing else (hand-set overrides win); a wrong stamp, no stamp or a directory that does not exist
#   supplies nothing; and a reconfigure without a valid directory forgets the paths an earlier configure cached (the
#   project caches them as FetchContent does).
# - scripts/ci-check-caches.sh, and scripts/ci-fetch-deps.sh from local tarballs through real FetchContent (with Ninja),
#   where bash is available, except on Windows.
# Usage: cmake -DREPO_DIR=<repo root> -DWORK_DIR=<scratch dir> -DGENERATOR=<generator> [-DMAKE_PROGRAM=<path>]
#        -P CheckDependencySources.cmake

set(problems "")
file(REMOVE_RECURSE "${WORK_DIR}")
set(project "${WORK_DIR}/Project")
set(pins "${project}/Pins.cmake")
set(generator -G "${GENERATOR}")
if(MAKE_PROGRAM)
	list(APPEND generator "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()

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

# The stamp scripts/ci-fetch-deps.sh writes for a pins file, with the same command.
function(stamp_of file outVar)
	execute_process(COMMAND "${CMAKE_COMMAND}" -E sha256sum "${file}" OUTPUT_VARIABLE line)
	string(REGEX MATCH "^[0-9a-f]+" stamp "${line}")
	set(${outVar} "${stamp}" PARENT_SCOPE)
endfunction()

# A source directory like the one scripts/ci-fetch-deps.sh fills, with a plain file and a build directory that must not
# count. stamp is "pins", "none" or a literal stamp; an extra "empty" argument leaves out the sources.
function(make_sources name stamp)
	set(dir "${WORK_DIR}/${name}")
	file(MAKE_DIRECTORY "${dir}")
	if(NOT ARGN STREQUAL "empty")
		file(MAKE_DIRECTORY "${dir}/glm-src" "${dir}/vulkanheaders-src" "${dir}/qux-build")
		file(WRITE "${dir}/file-src" "")
	endif()
	if(stamp STREQUAL "pins")
		stamp_of("${pins}" stamp)
	endif()
	if(NOT stamp STREQUAL "none")
		file(WRITE "${dir}/pins.sha256" "${stamp}\n")
	endif()
endfunction()

# Configures the project in tree (env: the source directory or "unset"; extra arguments go to cmake) and keeps its
# output in lastOutput.
function(configure tree env)
	if(env STREQUAL "unset")
		set(command "${CMAKE_COMMAND}" -E env --unset=BASALT_DEPS_SOURCE_DIR)
	else()
		set(command "${CMAKE_COMMAND}" -E env "BASALT_DEPS_SOURCE_DIR=${env}")
	endif()
	execute_process(COMMAND ${command} "${CMAKE_COMMAND}" ${generator} ${ARGN} -S "${project}" -B "${WORK_DIR}/${tree}"
		RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
	if(NOT result EQUAL 0)
		set(problems "${problems}\n${tree}: configure failed:\n${output}" PARENT_SCOPE)
	endif()
	set(lastOutput "${output}" PARENT_SCOPE)
endfunction()

# Records a problem unless the last configure overrode exactly the given names (with paths in sourceDir) and NAME=path
# pairs.
function(expect_overrides label sourceDir)
	foreach(name GLM VULKANHEADERS FILE QUX)
		string(TOLOWER "${name}" lower)
		set(wanted "")
		foreach(item IN LISTS ARGN)
			if(item STREQUAL name)
				set(wanted "${sourceDir}/${lower}-src")
			elseif(item MATCHES "^${name}=(.*)$")
				set(wanted "${CMAKE_MATCH_1}")
			endif()
		endforeach()
		string(REGEX MATCH "override ${name}=[^\n]*" line "${lastOutput}")
		if(NOT line STREQUAL "override ${name}=${wanted}")
			set(problems "${problems}\n${label}: expected 'override ${name}=${wanted}', got '${line}'")
		endif()
	endforeach()
	set(problems "${problems}" PARENT_SCOPE)
endfunction()

# Records a problem unless the last configure's output matches regex.
function(expect_output label regex)
	if(NOT lastOutput MATCHES "${regex}")
		set(problems "${problems}\n${label}: output does not match '${regex}':\n${lastOutput}" PARENT_SCOPE)
	endif()
endfunction()

make_sources(Stamped pins)
configure(Tree "${WORK_DIR}/Stamped")
expect_overrides(Stamped "${WORK_DIR}/Stamped" GLM VULKANHEADERS)
expect_output(Stamped "2 dependency sources from")

# The cached paths from the first configure must not outlive the variable,
configure(Tree unset)
expect_overrides(Reconfigured "")

# nor a stamp that stops matching while the variable is still set.
make_sources(WrongStamp 0123456789abcdef)
configure(StaleTree "${WORK_DIR}/Stamped")
configure(StaleTree "${WORK_DIR}/WrongStamp")
expect_overrides(StaleReconfigure "")
expect_output(StaleReconfigure "CMake Warning.*ignoring BASALT_DEPS_SOURCE_DIR")

make_sources(NoStamp none)
configure(NoStampTree "${WORK_DIR}/NoStamp")
expect_overrides(NoStamp "")
expect_output(NoStamp "CMake Warning.*ignoring BASALT_DEPS_SOURCE_DIR")

make_sources(Empty pins empty)
configure(EmptyTree "${WORK_DIR}/Empty")
expect_overrides(Empty "")
expect_output(Empty "CMake Warning.*holds no dependency sources")

# A cache miss in a workflow that does not fetch: the directory does not exist, which is not worth a warning.
configure(MissingTree "${WORK_DIR}/Missing")
expect_overrides(Missing "")
if(lastOutput MATCHES "Warning")
	set(problems "${problems}\nMissing: unexpected warning:\n${lastOutput}")
endif()

# CI passes Windows paths with backslashes; the overrides use forward slashes.
string(REPLACE "/" "\\" backslashed "${WORK_DIR}/Stamped")
configure(BackslashTree "${backslashed}")
expect_overrides(Backslashes "${WORK_DIR}/Stamped" GLM VULKANHEADERS)

# A hand-set override wins, and survives the next configure.
configure(HandSetTree "${WORK_DIR}/Stamped" "-DFETCHCONTENT_SOURCE_DIR_GLM=${WORK_DIR}/Mine")
expect_overrides(HandSet "${WORK_DIR}/Stamped" "GLM=${WORK_DIR}/Mine" VULKANHEADERS)
configure(HandSetTree unset)
expect_overrides(HandSetReconfigured "" "GLM=${WORK_DIR}/Mine")

find_program(bash bash)
if(bash AND NOT WIN32)
	# scripts/ci-check-caches.sh: a tree that downloaded a source fails and names it; one that downloaded none passes; a
	# launcher in the environment must match the tree's exactly, in LF and CRLF caches as large as real ones.
	file(MAKE_DIRECTORY "${WORK_DIR}/Downloaded/_deps/glm-src" "${WORK_DIR}/Used/_deps/glm-build" "${WORK_DIR}/UsedCrlf/_deps")
	# Real caches are tens of kilobytes, more than a pipe buffer holds, with the launchers well before the end.
	string(REPEAT "SOME_ENTRY:STRING=value\n" 5000 padding)
	file(WRITE "${WORK_DIR}/Downloaded/CMakeCache.txt" "${padding}")
	file(WRITE "${WORK_DIR}/Used/CMakeCache.txt"
		"CMAKE_C_COMPILER_LAUNCHER:STRING=ccache\nCMAKE_CXX_COMPILER_LAUNCHER:STRING=ccache\n${padding}")
	# As CMake writes it on Windows.
	string(REPLACE "\n" "\r\n" crlfPadding "${padding}")
	file(WRITE "${WORK_DIR}/UsedCrlf/CMakeCache.txt" "CMAKE_CXX_COMPILER_LAUNCHER:STRING=sccache\r\n${crlfPadding}")

	# Runs the check on tree with CMAKE_C/CXX_COMPILER_LAUNCHER set to the given values ("unset" for none).
	function(check tree cLauncher cxxLauncher expectPass pattern)
		set(env "")
		foreach(language C CXX)
			if(language STREQUAL "C")
				set(launcher "${cLauncher}")
			else()
				set(launcher "${cxxLauncher}")
			endif()
			if(launcher STREQUAL "unset")
				list(APPEND env "--unset=CMAKE_${language}_COMPILER_LAUNCHER")
			else()
				list(APPEND env "CMAKE_${language}_COMPILER_LAUNCHER=${launcher}")
			endif()
		endforeach()
		execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${env} "${bash}" "${REPO_DIR}/scripts/ci-check-caches.sh" "${WORK_DIR}/${tree}"
			RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
		set(label "ci-check-caches.sh ${tree} (C ${cLauncher}, CXX ${cxxLauncher})")
		if(expectPass AND NOT result EQUAL 0)
			set(problems "${problems}\n${label}: expected to pass, got ${result}:\n${output}" PARENT_SCOPE)
		elseif(NOT expectPass AND result EQUAL 0)
			set(problems "${problems}\n${label}: expected to fail, but passed:\n${output}" PARENT_SCOPE)
		elseif(NOT output MATCHES "${pattern}")
			set(problems "${problems}\n${label}: output does not match '${pattern}':\n${output}" PARENT_SCOPE)
		endif()
	endfunction()

	check(Downloaded unset unset FALSE "downloaded glm-src")
	check(Used unset unset TRUE "downloaded no dependency sources")
	check(Used ccache ccache TRUE "compiles CXX through ccache")
	check(Used unset sccache FALSE "does not compile CXX through sccache")
	check(Used sccache ccache FALSE "does not compile C through sccache")
	check(Used unset ccach FALSE "does not compile CXX through ccach")
	check(UsedCrlf unset sccache TRUE "compiles CXX through sccache")
	check(NoSuchTree unset unset FALSE "not a configured build tree")
	execute_process(COMMAND "${bash}" "${REPO_DIR}/scripts/ci-check-caches.sh" RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
	if(result EQUAL 0)
		set(problems "${problems}\nci-check-caches.sh: a missing argument was accepted")
	endif()

	# scripts/ci-fetch-deps.sh on a stand-in repository whose dependencies are local tarballs (one with a .git directory,
	# plus the two optional ones the fetch insists on): the sources land in the directory without git history, build or
	# subbuild directories, stamped for its Dependencies.cmake, and a configure with the directory uses all of them.
	find_program(ninja ninja)
	if(ninja)
		set(fake "${WORK_DIR}/FakeRepo")
		file(MAKE_DIRECTORY "${fake}/scripts" "${fake}/cmake" "${WORK_DIR}/Tarballs")
		file(COPY "${REPO_DIR}/scripts/ci-fetch-deps.sh" DESTINATION "${fake}/scripts")
		file(COPY "${REPO_DIR}/cmake/DependencySources.cmake" DESTINATION "${fake}/cmake")
		set(declarations "")
		foreach(dependency glm tracy nativefiledialog)
			file(WRITE "${WORK_DIR}/Content/${dependency}/${dependency}.h" "// ${dependency}\n")
			if(dependency STREQUAL "glm")
				file(WRITE "${WORK_DIR}/Content/${dependency}/.git/HEAD" "ref: refs/heads/main\n")
			endif()
			execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf "${WORK_DIR}/Tarballs/${dependency}.tar.gz" "${dependency}"
				WORKING_DIRECTORY "${WORK_DIR}/Content")
			string(APPEND declarations
				"FetchContent_Declare(${dependency} URL \"file://${WORK_DIR}/Tarballs/${dependency}.tar.gz\" SOURCE_SUBDIR _NoCMake)\n")
		endforeach()
		file(WRITE "${fake}/cmake/Dependencies.cmake" "include(FetchContent)
include(DependencySources)
basalt_use_dependency_sources(\"\${CMAKE_CURRENT_LIST_FILE}\")
${declarations}FetchContent_MakeAvailable(glm tracy nativefiledialog)
message(STATUS \"glm from \${glm_SOURCE_DIR}\")
")
		file(WRITE "${fake}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.25)
project(FakeRepo LANGUAGES NONE)
list(APPEND CMAKE_MODULE_PATH \"\${CMAKE_CURRENT_SOURCE_DIR}/cmake\")
include(Dependencies)
")
		set(fetched "${WORK_DIR}/Fetched")
		execute_process(COMMAND "${bash}" "${fake}/scripts/ci-fetch-deps.sh" "${fetched}"
			RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
		stamp_of("${fake}/cmake/Dependencies.cmake" expectedStamp)
		file(GLOB leftovers LIST_DIRECTORIES true "${fetched}/*-build" "${fetched}/*-subbuild" "${fetched}/*-src/.git")
		set(stamp "")
		if(EXISTS "${fetched}/pins.sha256")
			file(READ "${fetched}/pins.sha256" stamp)
			string(STRIP "${stamp}" stamp)
		endif()
		if(NOT result EQUAL 0)
			set(problems "${problems}\nci-fetch-deps.sh failed (${result}):\n${output}")
		elseif(NOT EXISTS "${fetched}/glm-src/glm.h" OR NOT EXISTS "${fetched}/tracy-src/tracy.h" OR leftovers)
			set(problems "${problems}\nci-fetch-deps.sh: wrong contents (leftovers: ${leftovers}):\n${output}")
		elseif(NOT stamp STREQUAL expectedStamp)
			set(problems "${problems}\nci-fetch-deps.sh: stamp '${stamp}', expected '${expectedStamp}'")
		else()
			execute_process(COMMAND "${CMAKE_COMMAND}" -E env "BASALT_DEPS_SOURCE_DIR=${fetched}"
				"${CMAKE_COMMAND}" -G Ninja "-DCMAKE_MAKE_PROGRAM=${ninja}" -S "${fake}" -B "${WORK_DIR}/FakeBuild"
				RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
			if(NOT result EQUAL 0 OR NOT output MATCHES "3 dependency sources from" OR NOT output MATCHES "glm from ${fetched}/glm-src")
				set(problems "${problems}\nA configure with the fetched directory did not use it (${result}):\n${output}")
			endif()
			check(FakeBuild unset unset TRUE "downloaded no dependency sources")
		endif()

		# Another fetch into the filled directory is refused and leaves it as it was.
		file(READ "${fetched}/pins.sha256" stampBefore)
		execute_process(COMMAND "${bash}" "${fake}/scripts/ci-fetch-deps.sh" "${fetched}"
			RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
		file(READ "${fetched}/pins.sha256" stampAfter)
		if(result EQUAL 0 OR NOT output MATCHES "is not empty" OR NOT stampBefore STREQUAL stampAfter)
			set(problems "${problems}\nci-fetch-deps.sh: a fetch into a filled directory was not refused (${result}):\n${output}")
		endif()

		# A fetch that leaves out an optional dependency fails without a stamp.
		file(WRITE "${fake}/cmake/Dependencies.cmake" "include(FetchContent)\n")
		execute_process(COMMAND "${bash}" "${fake}/scripts/ci-fetch-deps.sh" "${WORK_DIR}/Incomplete"
			RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
		if(result EQUAL 0 OR NOT output MATCHES "left out tracy" OR EXISTS "${WORK_DIR}/Incomplete/pins.sha256")
			set(problems "${problems}\nci-fetch-deps.sh: an incomplete fetch passed or wrote a stamp (${result}):\n${output}")
		endif()
	endif()
endif()

if(problems)
	message(FATAL_ERROR "Dependency source checks failed:${problems}")
endif()
message(STATUS "Dependency source checks passed")
