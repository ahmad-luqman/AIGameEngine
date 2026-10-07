# Third-party dependencies, fetched at configure time and pinned to exact tags/commits.
# Never float a dependency on a branch: every entry below must name a tag or a commit SHA.

include(FetchContent)

set(FETCHCONTENT_QUIET ON)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# Some dependencies still declare cmake_minimum_required < 3.5, which CMake 4.x rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Third-party code is built without our warning flags and never as shared libraries.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# ---------------------------------------------------------------------------------------------
# Declarations
# ---------------------------------------------------------------------------------------------

FetchContent_Declare(glfw
	GIT_REPOSITORY https://github.com/glfw/glfw.git
	GIT_TAG 3.4
	GIT_SHALLOW TRUE)

FetchContent_Declare(glm
	GIT_REPOSITORY https://github.com/g-truc/glm.git
	GIT_TAG 1.0.3
	GIT_SHALLOW TRUE)

# Vulkan headers are pinned here and shared with nvrhi so every platform compiles against the same API version.
FetchContent_Declare(VulkanHeaders
	GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
	GIT_TAG v1.4.352
	GIT_SHALLOW TRUE)

FetchContent_Declare(nvrhi
	GIT_REPOSITORY https://github.com/NVIDIA-RTX/NVRHI.git
	GIT_TAG 6b96fb03e07539f08327aea76c56d55f1de9d906)

FetchContent_Declare(spdlog
	GIT_REPOSITORY https://github.com/gabime/spdlog.git
	GIT_TAG v1.17.0
	GIT_SHALLOW TRUE)

FetchContent_Declare(entt
	GIT_REPOSITORY https://github.com/skypjack/entt.git
	GIT_TAG v3.16.0
	GIT_SHALLOW TRUE)

FetchContent_Declare(json
	GIT_REPOSITORY https://github.com/nlohmann/json.git
	GIT_TAG v3.12.0
	GIT_SHALLOW TRUE)

FetchContent_Declare(jolt
	GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
	GIT_TAG v5.6.0
	GIT_SHALLOW TRUE
	SOURCE_SUBDIR Build)

FetchContent_Declare(sol2
	GIT_REPOSITORY https://github.com/ThePhD/sol2.git
	GIT_TAG v3.5.0
	GIT_SHALLOW TRUE)

FetchContent_Declare(doctest
	GIT_REPOSITORY https://github.com/doctest/doctest.git
	GIT_TAG v2.5.3
	GIT_SHALLOW TRUE)

# Sources without a usable CMake project: populated only, targets defined below.
# SOURCE_SUBDIR points at a directory that does not exist so FetchContent never calls add_subdirectory.
FetchContent_Declare(lua
	GIT_REPOSITORY https://github.com/lua/lua.git
	GIT_TAG v5.4.9
	GIT_SHALLOW TRUE
	SOURCE_SUBDIR _BasaltNoCMake)

FetchContent_Declare(imgui
	GIT_REPOSITORY https://github.com/ocornut/imgui.git
	GIT_TAG v1.92.9-docking
	GIT_SHALLOW TRUE
	SOURCE_SUBDIR _BasaltNoCMake)

FetchContent_Declare(imguizmo
	GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
	GIT_TAG 18cef5e031d8c6973d80284c67f60549fafd78c1
	SOURCE_SUBDIR _BasaltNoCMake)

FetchContent_Declare(miniaudio
	GIT_REPOSITORY https://github.com/mackron/miniaudio.git
	GIT_TAG 0.11.25
	GIT_SHALLOW TRUE
	SOURCE_SUBDIR _BasaltNoCMake)

FetchContent_Declare(cgltf
	GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
	GIT_TAG v1.15
	GIT_SHALLOW TRUE
	SOURCE_SUBDIR _BasaltNoCMake)

FetchContent_Declare(stb
	GIT_REPOSITORY https://github.com/nothings/stb.git
	GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
	SOURCE_SUBDIR _BasaltNoCMake)

# ---------------------------------------------------------------------------------------------
# Options for CMake-based dependencies
# ---------------------------------------------------------------------------------------------

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_RTXMU OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_NVAPI OFF CACHE BOOL "" FORCE)
set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)
set(NVRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)

# Jolt: keep its defaults deterministic and quiet; link only through the Jolt target so JPH_* defines match.
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
set(CPP_EXCEPTIONS_ENABLED ON CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
# Jolt GPU compute (hair simulation) is unused and needs dxc; keep Jolt CPU-only.
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
# 32-bit object layers: Basalt packs motion type, collision layer and collision mask into them.
set(OBJECT_LAYER_BITS 32 CACHE STRING "" FORCE)

FetchContent_MakeAvailable(VulkanHeaders)
FetchContent_MakeAvailable(glfw glm nvrhi spdlog entt json jolt sol2 doctest
	lua imgui imguizmo miniaudio cgltf stb)

# ---------------------------------------------------------------------------------------------
# Targets for source-only dependencies
# ---------------------------------------------------------------------------------------------

# Lua 5.4 (compiled as C)
file(GLOB BASALT_LUA_SOURCES "${lua_SOURCE_DIR}/l*.c")
list(REMOVE_ITEM BASALT_LUA_SOURCES "${lua_SOURCE_DIR}/lua.c" "${lua_SOURCE_DIR}/luac.c" "${lua_SOURCE_DIR}/ltests.c" "${lua_SOURCE_DIR}/onelua.c")
add_library(BasaltLua STATIC ${BASALT_LUA_SOURCES})
target_include_directories(BasaltLua SYSTEM PUBLIC "${lua_SOURCE_DIR}")
set_target_properties(BasaltLua PROPERTIES LINKER_LANGUAGE C POSITION_INDEPENDENT_CODE ON)
if(UNIX AND NOT APPLE)
	target_compile_definitions(BasaltLua PRIVATE LUA_USE_LINUX)
	target_link_libraries(BasaltLua PRIVATE m dl)
elseif(APPLE)
	target_compile_definitions(BasaltLua PRIVATE LUA_USE_MACOSX)
endif()

# Dear ImGui (docking branch) + GLFW platform backend. The renderer backend is Basalt's own (nvrhi).
add_library(BasaltImGui STATIC
	"${imgui_SOURCE_DIR}/imgui.cpp"
	"${imgui_SOURCE_DIR}/imgui_demo.cpp"
	"${imgui_SOURCE_DIR}/imgui_draw.cpp"
	"${imgui_SOURCE_DIR}/imgui_tables.cpp"
	"${imgui_SOURCE_DIR}/imgui_widgets.cpp"
	"${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp"
	"${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp")
target_include_directories(BasaltImGui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends" "${imgui_SOURCE_DIR}/misc/cpp")
target_link_libraries(BasaltImGui PUBLIC glfw)
target_compile_definitions(BasaltImGui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)

add_library(BasaltImGuizmo STATIC "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
target_include_directories(BasaltImGuizmo SYSTEM PUBLIC "${imguizmo_SOURCE_DIR}/src")
target_link_libraries(BasaltImGuizmo PUBLIC BasaltImGui)

# Header-only single-file libraries; implementations are compiled once inside Basalt.
add_library(BasaltMiniaudio INTERFACE)
target_include_directories(BasaltMiniaudio SYSTEM INTERFACE "${miniaudio_SOURCE_DIR}")

add_library(BasaltCgltf INTERFACE)
target_include_directories(BasaltCgltf SYSTEM INTERFACE "${cgltf_SOURCE_DIR}")

add_library(BasaltStb INTERFACE)
target_include_directories(BasaltStb SYSTEM INTERFACE "${stb_SOURCE_DIR}")

# The Vulkan loader is opened dynamically at runtime (vulkan.hpp dynamic dispatch), so nothing links against it;
# Basalt only needs the Vulkan::Headers target.

unset(CMAKE_POLICY_VERSION_MINIMUM)
