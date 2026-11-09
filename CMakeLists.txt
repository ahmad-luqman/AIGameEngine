cmake_minimum_required(VERSION 3.25)

# The oldest macOS the engine, its tools and exported games run on (it must be set before project()). Every macOS build
# uses it, so a call into a newer API fails to compile on every machine instead of failing at run time on an older Mac.
# Exported Info.plists state it (LSMinimumSystemVersion), and scripts/ci-universal-vulkan.sh builds the release's Vulkan
# loader for it, reading the value from the set() line below: keep it a literal there. 13.3 is the first with the whole
# C++20 library Breccia uses (floating-point std::to_chars among it); the MoltenVK release that script pins declares an
# older minimum (12.0 at v1.4.2). A value given with -D or MACOSX_DEPLOYMENT_TARGET wins; an empty one (what CMake caches
# by itself, or an empty variable) does not.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
	set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(Breccia
	VERSION 0.1.0
	DESCRIPTION "Breccia - a simple, production-grade 3D game engine"
	LANGUAGES C CXX)

# The engine was called Basalt until PR #76. A tree configured before then keeps its options under the old prefix,
# which nothing reads: an ASan tree would quietly build without sanitizers, so such a configure stops here. The
# internal list of dependency source paths moves to its new name first (a failed configure still writes the cache), so
# breccia_use_dependency_sources clears the FetchContent entries it lists rather than taking them for hand-set ones.
if(DEFINED CACHE{BASALT_DEPS_SOURCE_PATHS})
	set(BRECCIA_DEPS_SOURCE_PATHS "$CACHE{BASALT_DEPS_SOURCE_PATHS}"
		CACHE INTERNAL "NAME=path of each source taken from BRECCIA_DEPS_SOURCE_DIR")
	unset(BASALT_DEPS_SOURCE_PATHS CACHE)
endif()
get_cmake_property(cacheVariables CACHE_VARIABLES)
list(FILTER cacheVariables INCLUDE REGEX "^BASALT_")
if(cacheVariables)
	list(JOIN cacheVariables ", " renamedOptions)
	message(FATAL_ERROR "This build tree predates the rename from Basalt to Breccia and still sets ${renamedOptions}. "
		"Delete the tree, or configure it again with -U \"BASALT_*\" and the same options under their BRECCIA_ names.")
endif()
if(NOT "$ENV{BASALT_DEPS_SOURCE_DIR}" STREQUAL "")
	message(WARNING "BASALT_DEPS_SOURCE_DIR is no longer read since the rename from Basalt to Breccia: "
		"set BRECCIA_DEPS_SOURCE_DIR instead.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (Debug, Release, Dist)" FORCE)
endif()

# "Dist" is the shipping configuration: optimized, no asserts, no editor-only code paths.
set(CMAKE_CXX_FLAGS_DIST "${CMAKE_CXX_FLAGS_RELEASE}" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_DIST "${CMAKE_C_FLAGS_RELEASE}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_DIST "${CMAKE_EXE_LINKER_FLAGS_RELEASE}" CACHE STRING "" FORCE)

option(BRECCIA_PROFILE "Enable Tracy CPU profiling (compiled out in Dist)" OFF)

option(BRECCIA_BUILD_EDITOR "Build the Breccia editor" ON)
option(BRECCIA_BUILD_RUNTIME "Build the Breccia runtime (exported game player)" ON)
option(BRECCIA_BUILD_TESTS "Build Breccia unit tests" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Sanitizers)
include(Coverage)
include(Dependencies)
include(CompilerWarnings)
include(Shaders)

add_subdirectory(Breccia)

if(BRECCIA_BUILD_EDITOR)
	add_subdirectory(Breccia-Editor)
endif()

if(BRECCIA_BUILD_RUNTIME)
	add_subdirectory(Breccia-Runtime)
endif()

add_subdirectory(Breccia-CLI)

if(BRECCIA_BUILD_TESTS)
	enable_testing()
	add_subdirectory(Tests)
endif()

if(BRECCIA_FUZZ)
	add_subdirectory(Tests/Fuzz)
endif()
