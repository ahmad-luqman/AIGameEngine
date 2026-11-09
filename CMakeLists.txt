cmake_minimum_required(VERSION 3.25)

# The oldest macOS the engine, its tools and exported games run on (it must be set before project()). Every macOS build
# uses it, so a call into a newer API fails to compile on every machine instead of failing at run time on an older Mac.
# Exported Info.plists state it (LSMinimumSystemVersion), and scripts/ci-universal-vulkan.sh builds the release's Vulkan
# loader for it, reading the value from the set() line below: keep it a literal there. 13.3 is the first with the whole
# C++20 library Basalt uses (floating-point std::to_chars among it); the MoltenVK release that script pins declares an
# older minimum (12.0 at v1.4.2). A value given with -D or MACOSX_DEPLOYMENT_TARGET wins; an empty one (what CMake caches
# by itself, or an empty variable) does not.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
	set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(Basalt
	VERSION 0.1.0
	DESCRIPTION "Basalt - a simple, production-grade 3D game engine"
	LANGUAGES C CXX)

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

option(BASALT_PROFILE "Enable Tracy CPU profiling (compiled out in Dist)" OFF)

option(BASALT_BUILD_EDITOR "Build the Basalt editor" ON)
option(BASALT_BUILD_RUNTIME "Build the Basalt runtime (exported game player)" ON)
option(BASALT_BUILD_TESTS "Build Basalt unit tests" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Sanitizers)
include(Coverage)
include(Dependencies)
include(CompilerWarnings)
include(Shaders)

add_subdirectory(Basalt)

if(BASALT_BUILD_EDITOR)
	add_subdirectory(Basalt-Editor)
endif()

if(BASALT_BUILD_RUNTIME)
	add_subdirectory(Basalt-Runtime)
endif()

add_subdirectory(Basalt-CLI)

if(BASALT_BUILD_TESTS)
	enable_testing()
	add_subdirectory(Tests)
endif()

if(BASALT_FUZZ)
	add_subdirectory(Tests/Fuzz)
endif()
