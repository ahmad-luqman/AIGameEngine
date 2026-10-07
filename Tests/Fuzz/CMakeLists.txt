# libFuzzer targets for untrusted input (enabled with -DBASALT_FUZZ=ON, Clang only). Run them with
# scripts/fuzz.sh; seed inputs live in Corpus/<Target>/.

foreach(target FuzzScene FuzzTexture FuzzGltf FuzzCommand FuzzLuaJson)
	add_executable(${target} ${target}.cpp FuzzCommon.h)
	basalt_set_warnings(${target})
	target_link_libraries(${target} PRIVATE Basalt BasaltStb)
	target_link_options(${target} PRIVATE -fsanitize=fuzzer)
endforeach()
