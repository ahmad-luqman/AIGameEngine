# Basalt Runtime: plays an exported game. Contains no editing functionality.

file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h")

add_executable(BasaltRuntime ${RUNTIME_SOURCES})
basalt_set_warnings(BasaltRuntime)
target_link_libraries(BasaltRuntime PRIVATE Basalt)
