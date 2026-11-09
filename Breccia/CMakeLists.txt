# Breccia engine core: static library linked by the editor, runtime, CLI and tests.

file(GLOB_RECURSE BRECCIA_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h")

add_library(Breccia STATIC ${BRECCIA_SOURCES})
breccia_set_warnings(Breccia)

target_include_directories(Breccia PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")

# Floating point must give the same bits on every platform and build type: physics replays and scene.hash
# are compared across platforms. No fused multiply-add contraction, which GCC (-ffp-contract=fast) and clang
# (=on) apply by default wherever the target has FMA, at the optimizer's discretion. PUBLIC so every target
# that compiles engine headers (inline glm and math code) agrees. /fp:precise is MSVC's default, pinned here;
# clang-cl also needs contraction turned off explicitly.
if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	target_compile_options(Breccia PUBLIC /fp:precise /clang:-ffp-contract=off)
elseif(MSVC)
	target_compile_options(Breccia PUBLIC /fp:precise)
else()
	target_compile_options(Breccia PUBLIC -ffp-contract=off)
endif()

target_compile_definitions(Breccia
	PUBLIC
		BR_VERSION="${PROJECT_VERSION}"
		GLM_FORCE_DEPTH_ZERO_TO_ONE
		GLM_FORCE_RADIANS
		GLM_ENABLE_EXPERIMENTAL
		GLFW_INCLUDE_NONE
		VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
		VULKAN_HPP_NO_SMART_HANDLE
		SOL_ALL_SAFETIES_ON=1
		$<$<CONFIG:Debug>:BR_DEBUG>
		$<$<CONFIG:Release>:BR_RELEASE>
		$<$<CONFIG:Dist>:BR_DIST>
		# windows.h must not define min/max macros (they break std::min/std::clamp).
		$<$<PLATFORM_ID:Windows>:NOMINMAX>
		$<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>)

target_link_libraries(Breccia
	PUBLIC
		glm::glm
		spdlog::spdlog
		EnTT::EnTT
		nlohmann_json::nlohmann_json
		nvrhi
		Vulkan::Headers
		BrecciaImGui
		BrecciaImGuizmo
		Jolt::Jolt
		sol2::sol2
		BrecciaLua
	PRIVATE
		glfw
		nvrhi_vk
		BrecciaMiniaudio
		BrecciaCgltf
		BrecciaStb
		meshoptimizer
		BrecciaTextureCodecs
		libzstd_static
		BrecciaEnet)

if(WIN32)
	# dbghelp: crash report stack traces and minidumps (Core/CrashReporter). shell32: CommandLineToArgvW (Core/Platform).
	target_link_libraries(Breccia PRIVATE ws2_32 dbghelp shell32)
endif()

file(GLOB BRECCIA_SHADER_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.vert"
	"${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.comp")

# One source for the C++/GLSL cascade array length and shadow-view index encoding.
set(BRECCIA_SHADOW_CASCADES 4)
# One scene-light budget for C++/GLSL. PUBLIC because LightClusters.h exposes buffer dimensions.
set(BRECCIA_MAX_LIGHTS 256)
target_compile_definitions(Breccia PUBLIC BR_MAX_LIGHTS=${BRECCIA_MAX_LIGHTS})
target_compile_definitions(Breccia PRIVATE BR_SHADOW_CASCADE_COUNT=${BRECCIA_SHADOW_CASCADES})
# Exported Info.plists state the macOS the runtime is built for (Project/Packaging.cpp). Defined on every platform (from
# the top-level default unless MACOSX_DEPLOYMENT_TARGET or -D says otherwise), so the Info.plist builder and its tests are
# the same everywhere.
target_compile_definitions(Breccia PRIVATE BR_MACOS_MINIMUM_VERSION="${CMAKE_OSX_DEPLOYMENT_TARGET}")

breccia_embed_shaders(Breccia
	INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Include"
	EXTRA_INCLUDE_DIRS "${fsr1_SOURCE_DIR}/ffx-fsr"
	SOURCES ${BRECCIA_SHADER_SOURCES})

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/Source" FILES ${BRECCIA_SOURCES})

# stb_image calls memcpy(NULL, src, 0) when a PNG's first IDAT chunk is empty (valid PNG; unfixed upstream).
# Copying zero bytes touches no memory, but glibc declares memcpy's pointers non-null, so UBSan's
# nonnull-attribute check would abort fuzzing and sanitizer runs on any such image. Only stb's translation
# unit is exempt; a null pointer with a non-zero size would still be caught by ASan as an invalid write.
if(BRECCIA_SANITIZE OR BRECCIA_FUZZ)
	set_source_files_properties(Source/Breccia/Asset/StbImplementation.cpp PROPERTIES COMPILE_OPTIONS -fno-sanitize=nonnull-attribute)
endif()

find_package(Threads REQUIRED)
target_link_libraries(Breccia PUBLIC Threads::Threads)
if(BRECCIA_PROFILE AND TARGET Tracy::TracyClient)
	set_target_properties(TracyClient PROPERTIES EXCLUDE_FROM_ALL TRUE)
	target_link_libraries(Breccia PUBLIC $<$<NOT:$<CONFIG:Dist>>:Tracy::TracyClient>)
	target_compile_definitions(Breccia PUBLIC $<$<NOT:$<CONFIG:Dist>>:BRECCIA_PROFILE>)
endif()
