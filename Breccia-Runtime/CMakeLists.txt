# Breccia Runtime: plays an exported game. Contains no editing functionality.

file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h")

add_executable(BrecciaRuntime ${RUNTIME_SOURCES})
breccia_set_warnings(BrecciaRuntime)
target_link_libraries(BrecciaRuntime PRIVATE Breccia)
