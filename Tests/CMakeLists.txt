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
# Lua Storage (the SceneSwitch scenes) saves under the user data directory: keep it in the build tree.
set_tests_properties(FeatureTest PROPERTIES ENVIRONMENT "BASALT_USER_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/UserData")

# GPU tests need a Vulkan device (a real GPU, or lavapipe on CI). They render the feature scene through the
# runtime with validation enabled; any Vulkan/nvrhi validation error fails the test. --offscreen needs no
# window or display server; --require-validation fails the test if the validation layer could not be loaded.
option(BASALT_GPU_TESTS "Register tests that need a GPU" OFF)
# Python is optional; headless checks also guard the GPU harness against false passes.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
	add_test(NAME LocalShadowHarness
		COMMAND "${Python3_EXECUTABLE}" -O "${CMAKE_CURRENT_SOURCE_DIR}/Rendering/LocalShadowsTests.py")
	set_tests_properties(LocalShadowHarness PROPERTIES LABELS unit TIMEOUT 30)
endif()

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
	# "Scaled" is the lit image rendered at half resolution and upsampled by the tonemap pass (render scale).
	# "Dynamic" reaches the same image through dynamic resolution: an unreachable frame-rate target drops the
	# scale to its 0.5 minimum (once GPU timer results arrive), and the scene renders into the top-left of
	# full-size targets. "ScaledSSAO"/"DynamicSSAO" are the same pair for the SSAO view, which isolates the
	# blur's edge handling. Each Dynamic image must equal its Scaled image exactly (compared below; exact
	# because 0.5 of 1280x720 makes every UV scale a power of two, so the taps land on the same texels).
	# Dynamic is also compared with the current driver's Scaled golden within image.compare's default
	# tolerance; the SSAO pair has no golden.
	set(dynamicArgs --dynamic-resolution 1000000 --min-render-scale 0.5)
	# Every driver these tests run on (lavapipe in CI, MoltenVK and desktop GPUs locally) has timer queries,
	# so missing GPU timings are a profiler bug. The prefixes come from FormatGpuTimings (pinned by a unit test).
	set(noGpuTimingsRegex "scene GPU time (unavailable|not available yet)")
	foreach(view Lit SSAO Normals Depth Scaled Dynamic ScaledSSAO DynamicSSAO)
		set(debugView ${view})
		set(renderTest FeatureTestRender_${view})
		set(referenceView ${view})
		set(extraArgs "")
		if(view STREQUAL "Lit")
			set(debugView None)
			set(renderTest FeatureTestRender)
		elseif(view STREQUAL "Scaled")
			set(debugView None)
			set(extraArgs --render-scale 0.5)
		elseif(view STREQUAL "Dynamic")
			set(debugView None)
			set(referenceView Scaled)
			set(extraArgs ${dynamicArgs})
		elseif(view STREQUAL "ScaledSSAO")
			set(debugView SSAO)
			set(referenceView "")
			set(extraArgs --render-scale 0.5)
		elseif(view STREQUAL "DynamicSSAO")
			set(debugView SSAO)
			set(referenceView "")
			set(extraArgs ${dynamicArgs})
		endif()
		add_test(NAME ${renderTest}
			COMMAND BasaltRuntime ${basaltRenderArgs} --offscreen --debug-view ${debugView} ${extraArgs} --screenshot "${basaltGoldenOutput}/FeatureTest_${view}.png")
		set_tests_properties(${renderTest} PROPERTIES FIXTURES_SETUP Render${view} FAIL_REGULAR_EXPRESSION "${noGpuTimingsRegex}")
		list(APPEND basaltGpuTests ${renderTest})

		if(BASALT_GOLDEN_DRIVER AND referenceView)
			set(reference "${CMAKE_CURRENT_SOURCE_DIR}/Data/Golden/${BASALT_GOLDEN_DRIVER}/FeatureTest_${referenceView}.png")
			add_test(NAME FeatureTestGolden_${view}
				COMMAND BasaltCLI image.compare "{\"actual\": \"${basaltGoldenOutput}/FeatureTest_${view}.png\", \"reference\": \"${reference}\", \"diff\": \"${basaltGoldenOutput}/FeatureTest_${view}.diff.png\", \"assertMatch\": true}")
			set_tests_properties(FeatureTestGolden_${view} PROPERTIES FIXTURES_REQUIRED Render${view} SKIP_REGULAR_EXPRESSION "reference image not found")
			list(APPEND basaltGpuTests FeatureTestGolden_${view})
		endif()
	endforeach()
	# Rendering into the top-left of larger targets must give exactly the image of rendering at that size;
	# a tap beyond the render area (stale texels) would change a few edge pixels. Driver-independent: both
	# images come from the same machine.
	foreach(pair "Dynamic;Scaled" "DynamicSSAO;ScaledSSAO")
		list(GET pair 0 dynamicView)
		list(GET pair 1 scaledView)
		add_test(NAME FeatureTest${dynamicView}MatchesScaled
			COMMAND BasaltCLI image.compare "{\"actual\": \"${basaltGoldenOutput}/FeatureTest_${dynamicView}.png\", \"reference\": \"${basaltGoldenOutput}/FeatureTest_${scaledView}.png\", \"diff\": \"${basaltGoldenOutput}/FeatureTest_${dynamicView}VsScaled.diff.png\", \"pixelThreshold\": 0, \"maxPercent\": 0, \"assertMatch\": true}")
		set_tests_properties(FeatureTest${dynamicView}MatchesScaled PROPERTIES FIXTURES_REQUIRED "Render${dynamicView};Render${scaledView}")
		list(APPEND basaltGpuTests FeatureTest${dynamicView}MatchesScaled)
	endforeach()
	# A dynamic run must actually have lowered the scale (the runtime also exits 6 when it could not run).
	# "1280x720" is the full size from basaltRenderArgs.
	set_tests_properties(FeatureTestRender_Dynamic FeatureTestRender_DynamicSSAO PROPERTIES FAIL_REGULAR_EXPRESSION "${noGpuTimingsRegex};rendered at 1280x720")
	# And when it cannot run (here: a minimum equal to the render scale leaves nothing to adjust), the run says
	# why and fails (exit code 6), so a test cannot pass without dynamic resolution having worked.
	add_test(NAME FeatureTestRender_DynamicInactive
		COMMAND BasaltRuntime ${basaltRenderArgs} --offscreen --dynamic-resolution 1000000 --min-render-scale 1)
	set_tests_properties(FeatureTestRender_DynamicInactive PROPERTIES PASS_REGULAR_EXPRESSION "did not run \\(inactive: nothing to adjust")
	list(APPEND basaltGpuTests FeatureTestRender_DynamicInactive)
	# The window + swapchain path needs a display, so it only runs on developer machines.
	if(NOT DEFINED ENV{CI})
		add_test(NAME FeatureTestRenderWindowed
			COMMAND BasaltRuntime ${basaltRenderArgs} --screenshot "${CMAKE_CURRENT_BINARY_DIR}/FeatureTestRenderWindowed.png")
		set_tests_properties(FeatureTestRenderWindowed PROPERTIES FAIL_REGULAR_EXPRESSION "${noGpuTimingsRegex}")
		list(APPEND basaltGpuTests FeatureTestRenderWindowed)
	endif()
	if(Python3_Interpreter_FOUND)
		add_test(NAME LocalShadowRender
			COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Rendering/LocalShadows.py"
				--runtime $<TARGET_FILE:BasaltRuntime> --cli $<TARGET_FILE:BasaltCLI>
				--output "${basaltGoldenOutput}/LocalShadows")
		set_tests_properties(LocalShadowRender PROPERTIES TIMEOUT 600)
		list(APPEND basaltGpuTests LocalShadowRender)
	else()
		message(STATUS "Python 3 not found: skipping LocalShadowRender (other GPU tests remain enabled)")
	endif()
	set_tests_properties(${basaltGpuTests} PROPERTIES LABELS gpu)
	if(APPLE)
		# Homebrew's validation layer manifest names its library without a path (Apple silicon and Intel prefixes).
		set_tests_properties(${basaltGpuTests} PROPERTIES ENVIRONMENT "DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib:/usr/local/lib")
	endif()
endif()

# Runtime option validation: bad values must stop the run before a window or GPU is needed (an ignored
# option would let a test pass without testing anything). The message is what is checked; a run that went
# ahead anyway logs "Runtime: playing" and fails, and --frames 1 plus the timeout keep it from hanging.
if(TARGET BasaltRuntime)
	set(basaltRuntimeOptionTests "")
	foreach(case
			"dynamic-resolution-low;--dynamic-resolution;5;--dynamic-resolution must be 0"
			"dynamic-resolution-high;--dynamic-resolution;2e6;--dynamic-resolution must be 0"
			"dynamic-resolution-text;--dynamic-resolution;abc;--dynamic-resolution must be 0"
			"dynamic-resolution-headless;--dynamic-resolution;60;--dynamic-resolution needs a GPU"
			"min-render-scale;--min-render-scale;0.1;--min-render-scale must be a number"
			"render-scale;--render-scale;3;--render-scale must be a number"
			"frames;--frames;abc;--frames must be an integer")
		list(GET case 0 caseName)
		list(GET case 1 option)
		list(GET case 2 value)
		list(GET case 3 message)
		add_test(NAME RuntimeOption_${caseName}
			COMMAND BasaltRuntime --project "${CMAKE_CURRENT_SOURCE_DIR}/Data/FeatureTest" --headless --frames 1 ${option} ${value})
		set_tests_properties(RuntimeOption_${caseName} PROPERTIES PASS_REGULAR_EXPRESSION "${message}")
		list(APPEND basaltRuntimeOptionTests RuntimeOption_${caseName})
	endforeach()
	# An option missing its value is fatal too, rather than running with the default (it must come last, or
	# it would take the next argument as its value).
	add_test(NAME RuntimeOption_missing-value
		COMMAND BasaltRuntime --project "${CMAKE_CURRENT_SOURCE_DIR}/Data/FeatureTest" --headless --frames 1 --dynamic-resolution)
	set_tests_properties(RuntimeOption_missing-value PROPERTIES PASS_REGULAR_EXPRESSION "option --dynamic-resolution requires a value")
	list(APPEND basaltRuntimeOptionTests RuntimeOption_missing-value)
	set_tests_properties(${basaltRuntimeOptionTests} PROPERTIES FAIL_REGULAR_EXPRESSION "Runtime: playing")
	# Accepted values run: 0 turns dynamic resolution off, which makes --min-render-scale pointless (a warning).
	add_test(NAME RuntimeOption_min-render-scale-unused
		COMMAND BasaltRuntime --project "${CMAKE_CURRENT_SOURCE_DIR}/Data/FeatureTest" --headless --frames 1 --dynamic-resolution 0 --min-render-scale 0.5)
	set_tests_properties(RuntimeOption_min-render-scale-unused PROPERTIES PASS_REGULAR_EXPRESSION "--min-render-scale has no effect")
	list(APPEND basaltRuntimeOptionTests RuntimeOption_min-render-scale-unused)
	set_tests_properties(${basaltRuntimeOptionTests} PROPERTIES LABELS integration TIMEOUT 60)
endif()

# Sample games built entirely through the automation API; their gameplay checks run headless.
add_test(NAME SampleTetris
	COMMAND BasaltCLI --project "${PROJECT_SOURCE_DIR}/Samples/Tetris" batch "${PROJECT_SOURCE_DIR}/Samples/Tetris/test_game.json")
add_test(NAME SampleJoints
	COMMAND BasaltCLI --project "${PROJECT_SOURCE_DIR}/Samples/Joints" batch "${PROJECT_SOURCE_DIR}/Samples/Joints/test_game.json")
# Determinism: a recorded session must replay to the identical state (same machine), and the committed
# replay must end on the same board on every platform (Samples/Tetris/test_replay.json).
add_test(NAME SampleTetrisReplay
	COMMAND BasaltCLI --project "${PROJECT_SOURCE_DIR}/Samples/Tetris" batch "${PROJECT_SOURCE_DIR}/Samples/Tetris/test_replay.json")

# Cross-platform determinism: a physics-heavy scene must reach the same state hashes on every platform
# and build type (Tests/Data/Determinism).
add_test(NAME PhysicsDeterminism
	COMMAND BasaltCLI --project "${PROJECT_SOURCE_DIR}/Tests/Data/Determinism" --no-save batch "${PROJECT_SOURCE_DIR}/Tests/Data/Determinism/Determinism.batch.json")

# Labels let CI jobs pick test groups (ctest -L gpu, ctest -LE "gpu|format").
set_tests_properties(BasaltTests PROPERTIES LABELS unit)
set(basaltIntegrationTests FeatureTest SampleTetris SampleTetrisReplay SampleJoints PhysicsDeterminism)
set_tests_properties(${basaltIntegrationTests} PROPERTIES LABELS integration)
if(TEST CodeFormat)
	set_tests_properties(CodeFormat PROPERTIES LABELS format)
endif()
if(BASALT_SANITIZE)
	set_property(TEST BasaltTests ${basaltIntegrationTests} APPEND PROPERTY ENVIRONMENT ${BASALT_SANITIZER_ENVIRONMENT})
endif()
