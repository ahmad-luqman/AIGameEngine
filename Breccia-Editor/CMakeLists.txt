# Breccia Editor: scene authoring GUI and automation server. Never shipped with games.

file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h")

add_executable(BrecciaEditor ${EDITOR_SOURCES})
breccia_set_warnings(BrecciaEditor)
target_include_directories(BrecciaEditor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_libraries(BrecciaEditor PRIVATE Breccia)

if(TARGET nfd)
	target_link_libraries(BrecciaEditor PRIVATE nfd)
	target_compile_definitions(BrecciaEditor PRIVATE BR_NATIVE_DIALOGS)
endif()
