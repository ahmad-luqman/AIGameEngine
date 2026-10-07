# Basalt Editor: scene authoring GUI and automation server. Never shipped with games.

file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h")

add_executable(BasaltEditor ${EDITOR_SOURCES})
basalt_set_warnings(BasaltEditor)
target_include_directories(BasaltEditor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_libraries(BasaltEditor PRIVATE Basalt)
