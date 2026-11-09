# breccia: headless command-line access to the automation API (project/scene authoring, testing, export).

file(GLOB_RECURSE CLI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h")

add_executable(BrecciaCLI ${CLI_SOURCES})
breccia_set_warnings(BrecciaCLI)
target_link_libraries(BrecciaCLI PRIVATE Breccia)
set_target_properties(BrecciaCLI PROPERTIES OUTPUT_NAME "breccia")
