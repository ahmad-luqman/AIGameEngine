# Basalt unit tests (doctest). Everything here runs headless: no window, no GPU.

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h")

add_executable(BasaltTests ${TEST_SOURCES})
basalt_set_warnings(BasaltTests)
target_link_libraries(BasaltTests PRIVATE Basalt doctest::doctest BasaltStb)
if(WIN32)
	target_link_libraries(BasaltTests PRIVATE ws2_32)
endif()
target_compile_definitions(BasaltTests PRIVATE BASALT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Data")

add_test(NAME BasaltTests COMMAND BasaltTests --reporters=console --no-intro=true WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# Formatting check. clang-format output differs between major versions, so CI enables this on one job only.
find_program(BASALT_CLANG_FORMAT clang-format)
option(BASALT_CHECK_FORMAT "Register the clang-format check as a test" ON)
if(BASALT_CHECK_FORMAT AND BASALT_CLANG_FORMAT)
	add_test(NAME CodeFormat COMMAND "${CMAKE_COMMAND}" -DCLANG_FORMAT=${BASALT_CLANG_FORMAT} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P "${PROJECT_SOURCE_DIR}/cmake/CheckFormat.cmake")
endif()

# Feature test: plays Tests/Data/FeatureTest headlessly through the basalt CLI. The scene uses every
# component and its Lua driver exercises the entire scripting API (see FeatureTest.lua).
add_test(NAME FeatureTest
	COMMAND BasaltCLI --project "${CMAKE_CURRENT_SOURCE_DIR}/Data/FeatureTest" batch "${CMAKE_CURRENT_SOURCE_DIR}/Data/FeatureTest/FeatureTest.batch.json")

# GPU tests need a Vulkan device (a real GPU, or lavapipe on CI). They render the feature scene through the
# runtime with validation enabled; any Vulkan/nvrhi validation error fails the test. --offscreen needs no
# window or display server; --require-validation fails the test if the validation layer could not be loaded.
option(BASALT_GPU_TESTS "Register tests that need a GPU" OFF)
if(BASALT_GPU_TESTS AND TARGET BasaltRuntime)
	set(basaltRenderArgs --project "${CMAKE_CURRENT_SOURCE_DIR}/Data/FeatureTest" --frames 90 --width 1280 --height 720 --require-validation)

	# Golden images: each debug view is rendered offscreen, then compared with the reference for this driver
	# family (Tests/Data/Golden/<driver>/). References differ per driver (rasterization and filtering are
	# not bit-exact across implementations); a missing reference skips the comparison. Update them with
	# scripts/update_golden.sh after checking the new images by eye.
	set(basaltDefaultGoldenDriver "")
	if(APPLE)
		set(basaltDefaultGoldenDriver moltenvk)
	endif()
	set(BASALT_GOLDEN_DRIVER "${basaltDefaultGoldenDriver}" CACHE STRING "Reference image set in Tests/Data/Golden to compare GPU renders with (empty: no comparison)")
	set(basaltGoldenOutput "${CMAKE_CURRENT_BINARY_DIR}/Golden")
	file(MAKE_DIRECTORY "${basaltGoldenOutput}")

	set(basaltGpuTests "")
	foreach(view Lit SSAO Normals Depth)
		set(debugView ${view})
		set(renderTest FeatureTestRender_${view})
		if(view STREQUAL "Lit")
			set(debugView None)
			set(renderTest FeatureTestRender)
		endif()
		add_test(NAME ${renderTest}
			COMMAND BasaltRuntime ${basaltRenderArgs} --offscreen --debug-view ${debugView} --screenshot "${basaltGoldenOutput}/FeatureTest_${view}.png")
		set_tests_properties(${renderTest} PROPERTIES FIXTURES_SETUP Render${view})
		list(APPEND basaltGpuTests ${renderTest})

		if(BASALT_GOLDEN_DRIVER)
			set(reference "${CMAKE_CURRENT_SOURCE_DIR}/Data/Golden/${BASALT_GOLDEN_DRIVER}/FeatureTest_${view}.png")
			add_test(NAME FeatureTestGolden_${view}
				COMMAND BasaltCLI image.compare "{\"actual\": \"${basaltGoldenOutput}/FeatureTest_${view}.png\", \"reference\": \"${reference}\", \"diff\": \"${basaltGoldenOutput}/FeatureTest_${view}.diff.png\", \"assertMatch\": true}")
			set_tests_properties(FeatureTestGolden_${view} PROPERTIES FIXTURES_REQUIRED Render${view} SKIP_REGULAR_EXPRESSION "reference image not found")
			list(APPEND basaltGpuTests FeatureTestGolden_${view})
		endif()
	endforeach()
	# The window + swapchain path needs a display, so it only runs on developer machines.
	if(NOT DEFINED ENV{CI})
		add_test(NAME FeatureTestRenderWindowed
			COMMAND BasaltRuntime ${basaltRenderArgs} --screenshot "${CMAKE_CURRENT_BINARY_DIR}/FeatureTestRenderWindowed.png")
		list(APPEND basaltGpuTests FeatureTestRenderWindowed)
	endif()
	set_tests_properties(${basaltGpuTests} PROPERTIES LABELS gpu)
	if(APPLE)
		# Homebrew's validation layer manifest names its library without a path (Apple silicon and Intel prefixes).
		set_tests_properties(${basaltGpuTests} PROPERTIES ENVIRONMENT "DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib:/usr/local/lib")
	endif()
endif()

# Sample games built entirely through the automation API; their gameplay checks run headless.
add_test(NAME SampleTetris
	COMMAND BasaltCLI --project "${PROJECT_SOURCE_DIR}/Samples/Tetris" batch "${PROJECT_SOURCE_DIR}/Samples/Tetris/test_game.json")
# Determinism: a recorded session must replay to the identical state (same machine), and the committed
# replay must end on the same board on every platform (Samples/Tetris/test_replay.json).
add_test(NAME SampleTetrisReplay
	COMMAND BasaltCLI --project "${PROJECT_SOURCE_DIR}/Samples/Tetris" batch "${PROJECT_SOURCE_DIR}/Samples/Tetris/test_replay.json")

# Labels let CI jobs pick test groups (ctest -L gpu, ctest -LE "gpu|format").
set_tests_properties(BasaltTests PROPERTIES LABELS unit)
set_tests_properties(FeatureTest SampleTetris SampleTetrisReplay PROPERTIES LABELS integration)
if(TEST CodeFormat)
	set_tests_properties(CodeFormat PROPERTIES LABELS format)
endif()
if(BASALT_SANITIZE)
	set_property(TEST BasaltTests FeatureTest SampleTetris SampleTetrisReplay APPEND PROPERTY ENVIRONMENT ${BASALT_SANITIZER_ENVIRONMENT})
endif()
