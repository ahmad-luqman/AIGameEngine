# GLSL -> SPIR-V compilation at build time with glslc (ships with the Vulkan SDK / shaderc).
# Compiled SPIR-V is embedded into the engine binary, so shipped games never look for shader files.

find_program(BASALT_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin" REQUIRED)

# basalt_embed_shaders(<target> INCLUDE_DIR <dir> SOURCES <files...>)
#
# Compiles every .vert/.frag/.comp source to SPIR-V and adds a generated EmbeddedShaders.cpp to <target>
# that registers them by file name (e.g. "ImGui.vert"). Shared headers (*.glsl in INCLUDE_DIR) are
# dependencies of every shader, so editing one recompiles everything that could include it.
function(basalt_embed_shaders target)
	cmake_parse_arguments(ARG "" "INCLUDE_DIR" "SOURCES" ${ARGN})

	set(outputDir "${CMAKE_CURRENT_BINARY_DIR}/EmbeddedShaders")
	file(MAKE_DIRECTORY "${outputDir}")

	set(includeDeps "")
	if(ARG_INCLUDE_DIR)
		file(GLOB includeDeps CONFIGURE_DEPENDS "${ARG_INCLUDE_DIR}/*.glsl")
	endif()

	set(generatedIncludes "")
	set(generatedTable "")
	set(outputs "")
	foreach(source IN LISTS ARG_SOURCES)
		get_filename_component(fileName "${source}" NAME)
		string(MAKE_C_IDENTIFIER "${fileName}" identifier)
		set(output "${outputDir}/${fileName}.inc")

		add_custom_command(
			OUTPUT "${output}"
			COMMAND "${BASALT_GLSLC}" --target-env=vulkan1.2 -O -Werror -mfmt=c
				-I "${ARG_INCLUDE_DIR}" "${source}" -o "${output}"
			DEPENDS "${source}" ${includeDeps}
			COMMENT "Compiling shader ${fileName}"
			VERBATIM)
		list(APPEND outputs "${output}")

		string(APPEND generatedIncludes "\tconst uint32_t s_${identifier}[] =\n#include \"${fileName}.inc\"\n\t;\n\n")
		string(APPEND generatedTable "\t\t\t{ \"${fileName}\", s_${identifier}, sizeof(s_${identifier}) },\n")
	endforeach()

	set(BASALT_SHADER_DEFINITIONS "${generatedIncludes}")
	set(BASALT_SHADER_TABLE "${generatedTable}")
	set(generatedSource "${outputDir}/EmbeddedShaders.cpp")
	configure_file("${PROJECT_SOURCE_DIR}/cmake/EmbeddedShaders.cpp.in" "${generatedSource}" @ONLY)

	# Shader compilation runs as part of building <target>; the generated .cpp depends on every blob.
	set_source_files_properties("${generatedSource}" PROPERTIES OBJECT_DEPENDS "${outputs}")
	add_custom_target(${target}Shaders DEPENDS ${outputs})
	add_dependencies(${target} ${target}Shaders)
	target_sources(${target} PRIVATE "${generatedSource}")
	target_include_directories(${target} PRIVATE "${outputDir}")
endfunction()
