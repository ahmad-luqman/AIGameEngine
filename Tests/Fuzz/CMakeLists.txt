# libFuzzer targets for untrusted input (enabled with -DBASALT_FUZZ=ON, Clang only). Run them with
# scripts/fuzz.sh; seed inputs live in Corpus/<Target>/. Every Fuzz*.cpp is a target, the same list that
# scripts/fuzz.sh --list gives the nightly workflow.

file(GLOB fuzzSources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Fuzz*.cpp")
foreach(source ${fuzzSources})
	get_filename_component(target ${source} NAME_WE)
	add_executable(${target} ${target}.cpp FuzzCommon.h)
	basalt_set_warnings(${target})
	target_link_libraries(${target} PRIVATE Basalt BasaltStb)
	target_link_options(${target} PRIVATE -fsanitize=fuzzer)
endforeach()
