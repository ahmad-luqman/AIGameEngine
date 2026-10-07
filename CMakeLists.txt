cmake_minimum_required(VERSION 3.25)

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

option(BASALT_BUILD_EDITOR "Build the Basalt editor" ON)
option(BASALT_BUILD_RUNTIME "Build the Basalt runtime (exported game player)" ON)
option(BASALT_BUILD_TESTS "Build Basalt unit tests" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

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
