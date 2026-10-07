# Basalt unit tests (doctest). Everything here runs headless: no window, no GPU.

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h")

add_executable(BasaltTests ${TEST_SOURCES})
basalt_set_warnings(BasaltTests)
target_link_libraries(BasaltTests PRIVATE Basalt doctest::doctest)
target_compile_definitions(BasaltTests PRIVATE BASALT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Data")

add_test(NAME BasaltTests COMMAND BasaltTests --reporters=console --no-intro=true WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# Formatting check. clang-format output differs between major versions, so CI enables this on one job only.
find_program(BASALT_CLANG_FORMAT clang-format)
option(BASALT_CHECK_FORMAT "Register the clang-format check as a test" ON)
if(BASALT_CHECK_FORMAT AND BASALT_CLANG_FORMAT)
	add_test(NAME CodeFormat COMMAND "${CMAKE_COMMAND}" -DCLANG_FORMAT=${BASALT_CLANG_FORMAT} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P "${PROJECT_SOURCE_DIR}/cmake/CheckFormat.cmake")
endif()
