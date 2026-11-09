# libFuzzer targets for untrusted input (enabled with -DBRECCIA_FUZZ=ON, Clang only). Run them with
# scripts/fuzz.sh; seed inputs live in Corpus/<Target>/. Every Fuzz*.cpp is a target, the same list that
# scripts/fuzz.sh --list gives the nightly workflow.

# Every target links the whole engine with sanitizers and debug information. Linking one per core at once ran a CI
# runner (4 cores, 16 GB) out of memory, which then shut down mid-build, so links take turns two at a time (Ninja).
set_property(GLOBAL APPEND PROPERTY JOB_POOLS breccia_fuzz_link=2)

file(GLOB fuzzSources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Fuzz*.cpp")
foreach(source ${fuzzSources})
	get_filename_component(target ${source} NAME_WE)
	add_executable(${target} ${target}.cpp FuzzCommon.h)
	breccia_set_warnings(${target})
	target_link_libraries(${target} PRIVATE Breccia BrecciaStb BrecciaMiniaudio)
	target_link_options(${target} PRIVATE -fsanitize=fuzzer)
	set_target_properties(${target} PROPERTIES JOB_POOL_LINK breccia_fuzz_link)
endforeach()
