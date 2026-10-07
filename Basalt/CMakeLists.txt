# Basalt engine core: static library linked by the editor, runtime, CLI and tests.

file(GLOB_RECURSE BASALT_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h")

add_library(Basalt STATIC ${BASALT_SOURCES})
basalt_set_warnings(Basalt)

target_include_directories(Basalt PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")

target_compile_definitions(Basalt
	PUBLIC
		GLM_FORCE_DEPTH_ZERO_TO_ONE
		GLM_FORCE_RADIANS
		GLM_ENABLE_EXPERIMENTAL
		GLFW_INCLUDE_NONE
		VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
		VULKAN_HPP_NO_SMART_HANDLE
		SOL_ALL_SAFETIES_ON=1
		$<$<CONFIG:Debug>:BS_DEBUG>
		$<$<CONFIG:Release>:BS_RELEASE>
		$<$<CONFIG:Dist>:BS_DIST>)

target_link_libraries(Basalt
	PUBLIC
		glm::glm
		spdlog::spdlog
		EnTT::EnTT
		nlohmann_json::nlohmann_json
		nvrhi
		Vulkan::Headers
		BasaltImGui
		BasaltImGuizmo
		Jolt::Jolt
		sol2::sol2
		BasaltLua
	PRIVATE
		glfw
		nvrhi_vk
		BasaltMiniaudio
		BasaltCgltf
		BasaltStb)

file(GLOB BASALT_SHADER_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.vert"
	"${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.comp")

basalt_embed_shaders(Basalt
	INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Include"
	SOURCES ${BASALT_SHADER_SOURCES})

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/Source" FILES ${BASALT_SOURCES})
