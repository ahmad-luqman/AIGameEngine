# basalt: headless command-line access to the automation API (project/scene authoring, testing, export).

file(GLOB_RECURSE CLI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h")

add_executable(BasaltCLI ${CLI_SOURCES})
basalt_set_warnings(BasaltCLI)
target_link_libraries(BasaltCLI PRIVATE Basalt)
set_target_properties(BasaltCLI PROPERTIES OUTPUT_NAME "basalt")
