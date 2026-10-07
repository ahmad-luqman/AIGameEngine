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
		$<$<CONFIG:Dist>:BS_DIST>
		# windows.h must not define min/max macros (they break std::min/std::clamp).
		$<$<PLATFORM_ID:Windows>:NOMINMAX>
		$<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>)

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

if(WIN32)
	target_link_libraries(Basalt PRIVATE ws2_32)
endif()

file(GLOB BASALT_SHADER_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.vert"
	"${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.comp")

basalt_embed_shaders(Basalt
	INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Include"
	SOURCES ${BASALT_SHADER_SOURCES})

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/Source" FILES ${BASALT_SOURCES})

# stb_image calls memcpy(NULL, src, 0) when a PNG's first IDAT chunk is empty (valid PNG; unfixed upstream).
# Copying zero bytes touches no memory, but glibc declares memcpy's pointers non-null, so UBSan's
# nonnull-attribute check would abort fuzzing and sanitizer runs on any such image. Only stb's translation
# unit is exempt; a null pointer with a non-zero size would still be caught by ASan as an invalid write.
if(BASALT_SANITIZE OR BASALT_FUZZ)
	set_source_files_properties(Source/Basalt/Asset/StbImplementation.cpp PROPERTIES COMPILE_OPTIONS -fno-sanitize=nonnull-attribute)
endif()
