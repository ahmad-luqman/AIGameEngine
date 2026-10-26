# Basalt unit tests (doctest). Everything here runs headless: no window, no GPU.

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h")

add_executable(BasaltTests ${TEST_SOURCES} "${PROJECT_SOURCE_DIR}/Basalt-Editor/Source/EditorContext.cpp" "${PROJECT_SOURCE_DIR}/Basalt-Editor/Source/Panels/JsonWidgets.cpp" "${PROJECT_SOURCE_DIR}/Basalt-Editor/Source/MaterialAssetDraft.cpp" "${PROJECT_SOURCE_DIR}/Basalt-Editor/Source/AnimationGraphDraft.cpp")
target_include_directories(BasaltTests PRIVATE "${PROJECT_SOURCE_DIR}/Basalt-Editor/Source")
basalt_set_warnings(BasaltTests)
target_link_libraries(BasaltTests PRIVATE Basalt doctest::doctest BasaltStb BasaltMiniaudio)
if(WIN32)
	target_link_libraries(BasaltTests PRIVATE ws2_32)
endif()
target_compile_definitions(BasaltTests PRIVATE BASALT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Data" BASALT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# BasaltTests runs as several ctest entries split by test file (doctest's --source-file filters), so `ctest -j` spreads
# the unit tests over the cores. Each shard below names files in Source/; the entry called BasaltTests runs every other
# file, so a new test file always runs. Split by measured time on CI's slowest test jobs (ASan, Windows Debug):
# rebalance when any entry clearly outlasts the rest. BasaltTests itself fails a run whose filters select no test case
# (TestMain.cpp), and the BasaltTestShards test checks that every test case runs exactly once.
set(basaltTestShards Editor Physics Animation Core)
set(basaltTestShard_Editor EditorHistoryTests EditorReviewTests)
set(basaltTestShard_Physics PhysicsTests CharacterControllerTests RagdollTests BenchmarkTests)
set(basaltTestShard_Animation AnimationTests SpringBoneTests MorphTargetTests InverseKinematicsTests RetargetingTests AnimationLayerTests)
set(basaltTestShard_Core CoreTests ScriptingTests AudioTests ProjectTests)
set(basaltUnitTests "")
set(basaltShardFiles "")
set(basaltShardPatterns "")
foreach(shard IN LISTS basaltTestShards)
	if(NOT basaltTestShard_${shard})
		# An empty --source-file filter selects every test case.
		message(FATAL_ERROR "BasaltTests shard ${shard} names no test files (basaltTestShard_${shard})")
	endif()
	set(patterns "")
	foreach(name IN LISTS basaltTestShard_${shard})
		if(name IN_LIST basaltShardFiles)
			message(FATAL_ERROR "BasaltTests shard ${shard}: ${name} is already in another shard")
		endif()
		list(APPEND basaltShardFiles ${name})
		# doctest matches a pattern against each test's full source path, ignoring case, and "*" crosses directory
		# separators (Windows paths use backslashes). So "*<name>.cpp" must end exactly one test file's path, or it would
		# also take another file into this shard (or into none, after a rename).
		string(TOLOWER "${name}" lowerName)
		set(matches "")
		foreach(source IN LISTS TEST_SOURCES)
			string(TOLOWER "${source}" lowerSource)
			if(lowerSource MATCHES "${lowerName}\\.cpp$")
				list(APPEND matches "${source}")
			endif()
		endforeach()
		if(NOT matches STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/Source/${name}.cpp")
			message(FATAL_ERROR "BasaltTests shard ${shard}: '*${name}.cpp' must match Source/${name}.cpp only, but matches: ${matches}")
		endif()
		list(APPEND patterns "*${name}.cpp")
	endforeach()
	list(JOIN patterns "," filter)
	add_test(NAME BasaltTests_${shard} COMMAND BasaltTests --reporters=console --no-intro=true "--source-file=${filter}" WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
	list(APPEND basaltUnitTests BasaltTests_${shard})
	list(APPEND basaltShardPatterns ${patterns})
endforeach()
list(JOIN basaltShardPatterns "," excluded)
add_test(NAME BasaltTests COMMAND BasaltTests --reporters=console --no-intro=true "--source-file-exclude=${excluded}" WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
list(APPEND basaltUnitTests BasaltTests)

set_tests_properties(${basaltUnitTests} PROPERTIES TIMEOUT 600)

# Formatting check. clang-format output differs between major versions, so CI enables this on one job only.
find_program(BASALT_CLANG_FORMAT clang-format)
option(BASALT_CHECK_FORMAT "Register the clang-format check as a test" ON)
if(BASALT_CHECK_FORMAT AND BASALT_CLANG_FORMAT)
	add_test(NAME CodeFormat COMMAND "${CMAKE_COMMAND}" -DCLANG_FORMAT=${BASALT_CLANG_FORMAT} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P "${PROJECT_SOURCE_DIR}/cmake/CheckFormat.cmake")
	# The check itself: batch boundaries, an empty tree and a clang-format that cannot start.
	add_test(NAME CheckFormatBatches COMMAND "${CMAKE_COMMAND}" -DCLANG_FORMAT=${BASALT_CLANG_FORMAT} -DREPO_DIR=${PROJECT_SOURCE_DIR} "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/CheckFormatBatches" -P "${CMAKE_CURRENT_SOURCE_DIR}/Format/CheckFormatBatches.cmake")
endif()

# CI's dependency-source cache (cmake/DependencySources.cmake, scripts/ci-fetch-deps.sh) and scripts/ci-check-caches.sh.
# Its small test projects use this build's generator.
add_test(NAME DependencySources COMMAND "${CMAKE_COMMAND}" -DREPO_DIR=${PROJECT_SOURCE_DIR} "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/DependencySources"
	"-DGENERATOR=${CMAKE_GENERATOR}" "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}" -P "${CMAKE_CURRENT_SOURCE_DIR}/Dependencies/CheckDependencySources.cmake")
set_tests_properties(DependencySources PROPERTIES LABELS unit TIMEOUT 60)

# scripts/fuzz.sh with stand-ins for cmake and the fuzzers: the nightly fuzz workflow's target list, input checks,
# running targets side by side (FUZZ_JOBS, also under macOS's bash 3.2) and how failures are reported. Bash is not part
# of the Windows toolchain.
if(NOT WIN32)
	add_test(NAME FuzzScript COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/Fuzz/CheckFuzzScript.sh" "${PROJECT_SOURCE_DIR}")
	set_tests_properties(FuzzScript PROPERTIES LABELS unit TIMEOUT 60)
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
	add_test(NAME PrefabFeature COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/Automation/PrefabEditing.py" $<TARGET_FILE:BasaltCLI> "${CMAKE_CURRENT_SOURCE_DIR}/Data/FeatureTest")
	set_tests_properties(PrefabFeature PROPERTIES LABELS integration TIMEOUT 90 ENVIRONMENT "BASALT_USER_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/PrefabUserData")
	add_test(NAME AssetPipelineFeature COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/Automation/AssetPipeline.py" $<TARGET_FILE:BasaltCLI> "${CMAKE_CURRENT_SOURCE_DIR}/Data/FeatureTest")
	set_tests_properties(AssetPipelineFeature PROPERTIES LABELS integration TIMEOUT 180 ENVIRONMENT "BASALT_USER_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/AssetPipelineUserData")

	# The BasaltTests entries above, as ctest registered them, run every test case exactly once.
	add_test(NAME BasaltTestShards COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/Shards/CheckBasaltTestShards.py"
		"${CMAKE_CTEST_COMMAND}" "${PROJECT_BINARY_DIR}" $<TARGET_FILE:BasaltTests> ${basaltUnitTests})
	set_tests_properties(BasaltTestShards PROPERTIES LABELS unit TIMEOUT 120)

	add_test(NAME BenchmarkHarness COMMAND "${Python3_EXECUTABLE}" -B -O "${CMAKE_CURRENT_SOURCE_DIR}/Benchmarks/RunTests.py")
	set_tests_properties(BenchmarkHarness PROPERTIES LABELS unit TIMEOUT 30)
	# scripts/clang_tidy.py (CI's sharded clang-tidy) against this build tree; its stand-in clang-tidy is a script with a
	# shebang, which Windows cannot start.
	if(NOT WIN32)
		add_test(NAME ClangTidyScript COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/ClangTidy/CheckClangTidyScript.py"
			"${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/ClangTidyScript")
		set_tests_properties(ClangTidyScript PROPERTIES LABELS unit TIMEOUT 60)
	endif()
	# Crashes child processes on purpose and checks their crash reports (Core/CrashReporter).
	add_test(NAME CrashReport COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/Automation/CrashReport.py" $<TARGET_FILE:BasaltTests>)
	set_tests_properties(CrashReport PROPERTIES LABELS integration TIMEOUT 300 SKIP_RETURN_CODE 77)
	add_test(NAME PhysicsJobFailure
		COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Automation/PhysicsJobFailure.py" $<TARGET_FILE:BasaltTests>)
	set_tests_properties(PhysicsJobFailure PROPERTIES LABELS unit TIMEOUT 90)
	add_test(NAME McpIntegration
		COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Automation/McpIntegration.py" $<TARGET_FILE:BasaltCLI>)
	set_tests_properties(McpIntegration PROPERTIES LABELS integration TIMEOUT 90 ENVIRONMENT "BASALT_USER_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/McpUserData")
	# Validate, export and smoke-test a project the way a release pipeline would; needs no GPU.
	if(TARGET BasaltRuntime)
		add_test(NAME ExportSmoke
			COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/Automation/ExportSmoke.py" $<TARGET_FILE:BasaltCLI>
				"${CMAKE_CURRENT_SOURCE_DIR}/Data/FeatureTest/Assets/Models/TexturedCube.glb")
		set_tests_properties(ExportSmoke PROPERTIES LABELS integration TIMEOUT 300)
		# The feature project exported as a folder and as Assets.bpak must play the same (Core/PackArchive).
		add_test(NAME ArchiveExport
			COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/Automation/ArchiveExport.py" $<TARGET_FILE:BasaltCLI> "${CMAKE_CURRENT_SOURCE_DIR}/Data/FeatureTest")
		set_tests_properties(ArchiveExport PROPERTIES LABELS integration TIMEOUT 600)
		# scripts/package_release.py as the release workflow runs it (one sample; the tools archive when the editor is built).
		set(releasePackagingArgs "")
		if(TARGET BasaltEditor)
			set(releasePackagingArgs --with-engine)
		endif()
		add_test(NAME ReleasePackaging COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/Automation/ReleasePackaging.py" $<TARGET_FILE:BasaltCLI> ${releasePackagingArgs})
		set_tests_properties(ReleasePackaging PROPERTIES LABELS integration TIMEOUT 900)
	endif()
	# Docs/AutomationCommands.md, Docs/Components.md and basalt.d.lua's component section must match the engine.
	add_test(NAME GeneratedDocs
		COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/scripts/generate_docs.py" --check $<TARGET_FILE:BasaltCLI>)
	set_tests_properties(GeneratedDocs PROPERTIES LABELS unit TIMEOUT 60)
	add_test(NAME LocalShadowHarness
		COMMAND "${Python3_EXECUTABLE}" -O "${CMAKE_CURRENT_SOURCE_DIR}/Rendering/LocalShadowsTests.py")
	set_tests_properties(LocalShadowHarness PROPERTIES LABELS unit TIMEOUT 30)
endif()

if(BASALT_GPU_TESTS AND TARGET BasaltRuntime)
	function(basalt_add_gpu_test name source timeout)
		add_executable(Basalt${name} ${source})
		basalt_set_warnings(Basalt${name})
		target_link_libraries(Basalt${name} PRIVATE Basalt doctest::doctest)
		add_test(NAME ${name} COMMAND Basalt${name})
		set_tests_properties(${name} PROPERTIES TIMEOUT ${timeout})
	endfunction()
	basalt_add_gpu_test(LightClustersGPU Rendering/LightClustersGPU.cpp 120)
	basalt_add_gpu_test(ViewportGPU Rendering/ViewportGPU.cpp 120)
	basalt_add_gpu_test(PreviewRendererGPU Rendering/PreviewRendererGPU.cpp 120)
	file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Previews")
	target_compile_definitions(BasaltPreviewRendererGPU PRIVATE BASALT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Data" BASALT_PREVIEW_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/Previews")
	target_compile_definitions(BasaltViewportGPU PRIVATE BASALT_ANIMATION_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Data/Animation")
	basalt_add_gpu_test(FSR1GPU Rendering/FSR1GPU.cpp 60)
	basalt_add_gpu_test(SSAOBlurGPU Rendering/SSAOBlurGPU.cpp 60)
	basalt_add_gpu_test(PostProcessGPU Rendering/PostProcessGPU.cpp 120)
	target_compile_definitions(BasaltPostProcessGPU PRIVATE BASALT_POSTPROCESS_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/PostProcess")
	basalt_add_gpu_test(TemporalAAGPU Rendering/TemporalAAGPU.cpp 120)
	target_sources(BasaltTemporalAAGPU PRIVATE Rendering/TemporalFailureGPU.cpp)
	target_compile_definitions(BasaltTemporalAAGPU PRIVATE BASALT_ANIMATION_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Data/Animation")
	basalt_add_gpu_test(AnimationGPU Rendering/AnimationGPU.cpp 120)
	target_compile_definitions(BasaltAnimationGPU PRIVATE BASALT_ANIMATION_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Data/Animation" BASALT_ANIMATION_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/Animation")
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

	set(basaltGpuTests TemporalAAGPU PostProcessGPU ViewportGPU FSR1GPU SSAOBlurGPU LightClustersGPU AnimationGPU PreviewRendererGPU)
	# "Scaled" is the lit image rendered at half resolution and upsampled by the tonemap pass (render scale).
	# "Dynamic" reaches the same image through dynamic resolution: an unreachable frame-rate target drops the
	# scale to its 0.5 minimum (once GPU timer results arrive), and the scene renders into the top-left of
	# full-size targets. "ScaledSSAO"/"DynamicSSAO" are the same pair for the SSAO view, which isolates the
	# blur's edge handling, and "ScaledSSAO60"/"DynamicSSAO60" repeat it at a non-dyadic 0.6. Each Dynamic image must
	# equal its Scaled image exactly (compared below). The SSAO passes address the render area by texel, so their pairs
	# are exact at any scale; the lit pair stays at 0.5, where every UV scale is a power of two, because the lit image's
	# filtered taps are exact only there. Dynamic is also compared with the current driver's Scaled golden within
	# image.compare's default tolerance; the SSAO pairs have no golden.
	set(dynamicArgs --dynamic-resolution 1000000 --min-render-scale 0.5)
	# Every driver these tests run on (lavapipe in CI, MoltenVK and desktop GPUs locally) has timer queries,
	# so missing GPU timings are a profiler bug. The prefixes are logged by RuntimeLayer::CheckRendererResults.
	set(noGpuTimingsRegex "(scene|blit|ImGui) GPU time (unavailable|not available yet|stalled)")
	foreach(view Lit SSAO Normals Depth Scaled Dynamic ScaledSSAO DynamicSSAO ScaledSSAO60 DynamicSSAO60)
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
		elseif(view STREQUAL "ScaledSSAO60")
			set(debugView SSAO)
			set(referenceView "")
			set(extraArgs --render-scale 0.6)
		elseif(view STREQUAL "DynamicSSAO60")
			set(debugView SSAO)
			set(referenceView "")
			set(extraArgs --dynamic-resolution 1000000 --min-render-scale 0.6)
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
	foreach(pair "Dynamic;Scaled" "DynamicSSAO;ScaledSSAO" "DynamicSSAO60;ScaledSSAO60")
		list(GET pair 0 dynamicView)
		list(GET pair 1 scaledView)
		add_test(NAME FeatureTest${dynamicView}MatchesScaled
			COMMAND BasaltCLI image.compare "{\"actual\": \"${basaltGoldenOutput}/FeatureTest_${dynamicView}.png\", \"reference\": \"${basaltGoldenOutput}/FeatureTest_${scaledView}.png\", \"diff\": \"${basaltGoldenOutput}/FeatureTest_${dynamicView}VsScaled.diff.png\", \"pixelThreshold\": 0, \"maxPercent\": 0, \"assertMatch\": true}")
		set_tests_properties(FeatureTest${dynamicView}MatchesScaled PROPERTIES FIXTURES_REQUIRED "Render${dynamicView};Render${scaledView}")
		list(APPEND basaltGpuTests FeatureTest${dynamicView}MatchesScaled)
	endforeach()
	# A dynamic run must actually have lowered the scale (the runtime also exits 6 when it could not run).
	# "1280x720" is the full size from basaltRenderArgs.
	set_tests_properties(FeatureTestRender_Dynamic FeatureTestRender_DynamicSSAO FeatureTestRender_DynamicSSAO60 PROPERTIES FAIL_REGULAR_EXPRESSION "${noGpuTimingsRegex};rendered at 1280x720")
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
		# Alone, so while it presents and reads back its swapchain no other test's window (EditorViewport's) covers it on
		# a developer's desktop: scripts/test.sh runs tests side by side.
		set_tests_properties(FeatureTestRenderWindowed PROPERTIES FAIL_REGULAR_EXPRESSION "${noGpuTimingsRegex}" RUN_SERIAL TRUE)
		list(APPEND basaltGpuTests FeatureTestRenderWindowed)
	endif()
	if(Python3_Interpreter_FOUND)
		add_test(NAME UIFollowupsRender
			COMMAND "${Python3_EXECUTABLE}" -B -O "${CMAKE_CURRENT_SOURCE_DIR}/Rendering/UIFollowups.py"
				$<TARGET_FILE:BasaltRuntime> "${CMAKE_CURRENT_BINARY_DIR}/UIFollowups")
		set_tests_properties(UIFollowupsRender PROPERTIES TIMEOUT 120)
		list(APPEND basaltGpuTests UIFollowupsRender)
		# Exercise the actual editor host. Linux CI supplies a virtual display; macOS uses the desktop.
		set(basaltEditorDisplay "")
		if(UNIX AND NOT APPLE)
			find_program(BASALT_XVFB_RUN xvfb-run)
			if(BASALT_XVFB_RUN)
				set(basaltEditorDisplay "${BASALT_XVFB_RUN}" -a -s "-screen 0 1280x900x24")
			endif()
		endif()
		if(TARGET BasaltEditor AND (APPLE OR WIN32 OR BASALT_XVFB_RUN OR DEFINED ENV{DISPLAY}))
			add_test(NAME EditorViewport
				COMMAND ${basaltEditorDisplay} "${Python3_EXECUTABLE}" -B -O "${CMAKE_CURRENT_SOURCE_DIR}/Automation/EditorViewport.py"
					$<TARGET_FILE:BasaltEditor> $<TARGET_FILE:BasaltCLI> "${CMAKE_CURRENT_BINARY_DIR}/Viewport")
			# Includes the Game view, thumbnail (software-rendered on lavapipe) and project settings checks.
			set_tests_properties(EditorViewport PROPERTIES TIMEOUT 300)
			list(APPEND basaltGpuTests EditorViewport)
		elseif(TARGET BasaltEditor)
			message(WARNING "EditorViewport is not registered: install xvfb-run or provide DISPLAY for the editor integration test")
		endif()
		# -B keeps helper bytecode out of the source tree; -O verifies checks survive Python optimization.
		add_test(NAME StatisticsOverlay
			COMMAND "${Python3_EXECUTABLE}" -B -O "${CMAKE_CURRENT_SOURCE_DIR}/Rendering/StatisticsOverlay.py"
				$<TARGET_FILE:BasaltRuntime> "${CMAKE_CURRENT_BINARY_DIR}/StatisticsOverlay")
		set_tests_properties(StatisticsOverlay PROPERTIES TIMEOUT 240)
		list(APPEND basaltGpuTests StatisticsOverlay)
		add_test(NAME FSR1Render
			COMMAND "${Python3_EXECUTABLE}" -B -O "${CMAKE_CURRENT_SOURCE_DIR}/Rendering/FSR1.py"
				--runtime $<TARGET_FILE:BasaltRuntime> --cli $<TARGET_FILE:BasaltCLI>
				--output "${basaltGoldenOutput}/FSR1")
		set_tests_properties(FSR1Render PROPERTIES TIMEOUT 600)
		list(APPEND basaltGpuTests FSR1Render)
		add_test(NAME LocalShadowRender
			COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Rendering/LocalShadows.py"
				--runtime $<TARGET_FILE:BasaltRuntime> --cli $<TARGET_FILE:BasaltCLI>
				--output "${basaltGoldenOutput}/LocalShadows")
		set_tests_properties(LocalShadowRender PROPERTIES TIMEOUT 600)
		list(APPEND basaltGpuTests LocalShadowRender)
	else()
		message(STATUS "Python 3 not found: skipping LocalShadowRender and FSR1Render (other GPU tests remain enabled)")
	endif()
	set_tests_properties(${basaltGpuTests} PROPERTIES LABELS gpu)
	# Explicitly enable synchronization validation for the compute-write -> fragment-read path.
	set_property(TEST TemporalAAGPU PostProcessGPU LightClustersGPU ViewportGPU PreviewRendererGPU APPEND PROPERTY ENVIRONMENT "VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT")
endif()

# Runtime option validation: bad values must stop the run before a window or GPU is needed (an ignored
# option would let a test pass without testing anything). The message is what is checked; a run that went
# ahead anyway logs "Runtime: playing" and fails, and --frames 1 plus the timeout keep it from hanging.
if(TARGET BasaltRuntime)
	set(basaltRuntimeOptionTests "")
	foreach(case
			"stats-headless;--stats;;--stats needs a GPU"
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
	# Script errors never stop a game, so only --fail-on-script-errors turns them into a failed run (exit code 7,
	# what the export smoke test relies on); without it the same run exits 0.
	add_test(NAME RuntimeOption_fail-on-script-errors
		COMMAND "${CMAKE_COMMAND}" "-DCOMMAND=$<TARGET_FILE:BasaltRuntime>;--project;${CMAKE_CURRENT_SOURCE_DIR}/Data/ScriptError;--headless;--frames;5;--fail-on-script-errors"
			-DEXPECTED=7 "-DMATCH=[0-9]+ script error[(]s[)] occurred; the first: .*deliberate script error" -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectExitCode.cmake")
	add_test(NAME RuntimeOption_script-errors-ignored
		COMMAND "${CMAKE_COMMAND}" "-DCOMMAND=$<TARGET_FILE:BasaltRuntime>;--project;${CMAKE_CURRENT_SOURCE_DIR}/Data/ScriptError;--headless;--frames;5"
			-DEXPECTED=0 "-DMATCH=deliberate script error" -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectExitCode.cmake")
	add_test(NAME RuntimeOption_fail-on-script-errors-needs-frames
		COMMAND "${CMAKE_COMMAND}" "-DCOMMAND=$<TARGET_FILE:BasaltRuntime>;--project;${CMAKE_CURRENT_SOURCE_DIR}/Data/ScriptError;--headless;--fail-on-script-errors"
			-DEXPECTED=1 "-DMATCH=--fail-on-script-errors needs --frames" -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectExitCode.cmake")
	# Errors OnDestroy raises as the run ends count too.
	add_test(NAME RuntimeOption_fail-on-script-errors-ondestroy
		COMMAND "${CMAKE_COMMAND}" "-DCOMMAND=$<TARGET_FILE:BasaltRuntime>;--project;${CMAKE_CURRENT_SOURCE_DIR}/Data/ScriptError;--scene;Assets/Scenes/OnDestroy.bscene;--headless;--frames;5;--fail-on-script-errors"
			-DEXPECTED=7 "-DMATCH=1 script error[(]s[)] occurred; the first: .*deliberate OnDestroy error" -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectExitCode.cmake")
	list(APPEND basaltRuntimeOptionTests RuntimeOption_fail-on-script-errors RuntimeOption_script-errors-ignored RuntimeOption_fail-on-script-errors-needs-frames RuntimeOption_fail-on-script-errors-ondestroy)
	set_tests_properties(${basaltRuntimeOptionTests} PROPERTIES LABELS integration TIMEOUT 60)
endif()

# Sample games built entirely through the automation API; their gameplay checks run headless.
add_test(NAME SampleTetris
	COMMAND BasaltCLI --project "${PROJECT_SOURCE_DIR}/Samples/Tetris" batch "${PROJECT_SOURCE_DIR}/Samples/Tetris/test_game.json")
add_test(NAME SampleJoints
	COMMAND BasaltCLI --project "${PROJECT_SOURCE_DIR}/Samples/Joints" batch "${PROJECT_SOURCE_DIR}/Samples/Joints/test_game.json")
# Marble Run's gameplay checks are mostly automation assertions (assert.*, events.poll expect, scene.diff); see
# Samples/MarbleRun/README.md.
add_test(NAME SampleMarbleRun
	COMMAND BasaltCLI --project "${PROJECT_SOURCE_DIR}/Samples/MarbleRun" batch "${PROJECT_SOURCE_DIR}/Samples/MarbleRun/test_game.json")
# Breakout drives its paddle through input actions (keys, a gamepad, the mouse, a Lua rebinding); see
# Samples/Breakout/README.md.
add_test(NAME SampleBreakout
	COMMAND BasaltCLI --project "${PROJECT_SOURCE_DIR}/Samples/Breakout" batch "${PROJECT_SOURCE_DIR}/Samples/Breakout/test_game.json")
# First Person: mouse and stick look with a locked cursor, a character controller, raycast shooting, and the audio
# effects (occlusion low-pass, the hall's reverb, the drone's Doppler); see Samples/FirstPerson/README.md.
add_test(NAME SampleFirstPerson
	COMMAND BasaltCLI --project "${PROJECT_SOURCE_DIR}/Samples/FirstPerson" batch "${PROJECT_SOURCE_DIR}/Samples/FirstPerson/test_game.json")
# Layers and inverse kinematics on a walking mannequin: foot placement, look-at, a masked wave and a reach; see
# Samples/MovementDemo/README.md.
add_test(NAME SampleMovementDemo
	COMMAND BasaltCLI --project "${PROJECT_SOURCE_DIR}/Samples/MovementDemo" batch "${PROJECT_SOURCE_DIR}/Samples/MovementDemo/test_game.json")
# A platformer course: a character controller with a state machine, root motion, foot placement, spring bones and a
# moving platform; see Samples/Platformer/README.md.
add_test(NAME SamplePlatformer
	COMMAND BasaltCLI --project "${PROJECT_SOURCE_DIR}/Samples/Platformer" batch "${PROJECT_SOURCE_DIR}/Samples/Platformer/test_game.json")
# Determinism: a recorded session must replay to the identical state (same machine), and the committed
# replay must end on the same board on every platform (Samples/Tetris/test_replay.json).
add_test(NAME SampleTetrisReplay
	COMMAND BasaltCLI --project "${PROJECT_SOURCE_DIR}/Samples/Tetris" batch "${PROJECT_SOURCE_DIR}/Samples/Tetris/test_replay.json")

# Cross-platform determinism: a physics-heavy scene must reach the same state hashes on every platform
# and build type (Tests/Data/Determinism).
add_test(NAME PhysicsDeterminism
	COMMAND BasaltCLI --project "${PROJECT_SOURCE_DIR}/Tests/Data/Determinism" --no-save batch "${PROJECT_SOURCE_DIR}/Tests/Data/Determinism/Determinism.batch.json")

# Labels let CI jobs pick test groups (ctest -L gpu, ctest -LE "gpu|format").
set_tests_properties(${basaltUnitTests} PROPERTIES LABELS unit)
set(basaltIntegrationTests FeatureTest SampleTetris SampleTetrisReplay SampleJoints SampleMarbleRun SampleBreakout SampleFirstPerson SampleMovementDemo SamplePlatformer PhysicsDeterminism)
set_tests_properties(${basaltIntegrationTests} PROPERTIES LABELS integration)
if(TEST CodeFormat)
	set_tests_properties(CodeFormat CheckFormatBatches PROPERTIES LABELS format)
endif()
if(BASALT_SANITIZE)
	set_property(TEST ${basaltUnitTests} ${basaltIntegrationTests} APPEND PROPERTY ENVIRONMENT ${BASALT_SANITIZER_ENVIRONMENT})
endif()

# Release benchmarks use the same fixed workload on every runner; timing gates are opt-in locally.
add_executable(BasaltBenchmark Benchmarks/Benchmark.cpp)
basalt_set_warnings(BasaltBenchmark)
target_link_libraries(BasaltBenchmark PRIVATE Basalt)
target_compile_definitions(BasaltBenchmark PRIVATE BASALT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
	BASALT_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}" BASALT_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
option(BASALT_BENCHMARK_TESTS "Run Release frame-time regression gates" OFF)
if(BASALT_BENCHMARK_TESTS)
	if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
		message(FATAL_ERROR "BASALT_BENCHMARK_TESTS requires a Release build")
	endif()
	if(NOT Python3_Interpreter_FOUND)
		message(FATAL_ERROR "BASALT_BENCHMARK_TESTS requires Python 3")
	endif()
	foreach(mode cpu gpu)
		if(mode STREQUAL "gpu" AND NOT BASALT_GPU_TESTS)
			message(STATUS "Benchmark_gpu is not registered: enable BASALT_GPU_TESTS to run the GPU gate")
			continue()
		endif()
		add_test(NAME Benchmark_${mode}
			COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/Benchmarks/Run.py"
				--binary $<TARGET_FILE:BasaltBenchmark> --mode ${mode}
				--output "${CMAKE_CURRENT_BINARY_DIR}/Benchmark-${mode}.json")
		# Alone on the machine, so `ctest -j` never skews the recorded frame times.
		set_tests_properties(Benchmark_${mode} PROPERTIES LABELS benchmark TIMEOUT 300 RUN_SERIAL TRUE)
		if(mode STREQUAL "gpu")
			set_property(TEST Benchmark_${mode} APPEND PROPERTY LABELS gpu)
			# lavapipe uses every core unless LP_NUM_THREADS (CI's limit for GPU tests run side by side) says otherwise.
			set_property(TEST Benchmark_${mode} PROPERTY ENVIRONMENT_MODIFICATION "LP_NUM_THREADS=unset:")
		endif()
	endforeach()
endif()
