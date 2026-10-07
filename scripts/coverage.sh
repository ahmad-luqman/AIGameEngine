#!/usr/bin/env bash
# Builds an instrumented Debug build into build-coverage/, runs every headless test, and writes a
# coverage report: a per-file summary on stdout (also saved as summary.txt) and HTML in
# build-coverage/coverage/html. Coverage guides where tests are thin; it is not a gate.
set -euo pipefail
cd "$(dirname "$0")/.."
buildDir="build-coverage"

# Prefer the toolchain's own llvm tools: profiles must come from the same LLVM version as the compiler.
if [[ "$(uname)" == "Darwin" ]]; then
	profdata=(xcrun llvm-profdata)
	cov=(xcrun llvm-cov)
else
	profdata=(llvm-profdata)
	cov=(llvm-cov)
fi

cmake -S . -B "$buildDir" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
	-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
	-DBASALT_COVERAGE=ON -DBASALT_CHECK_FORMAT=OFF -DBASALT_GPU_TESTS=OFF
cmake --build "$buildDir"

profileDir="$PWD/$buildDir/profiles"
rm -rf "$profileDir"
mkdir -p "$profileDir"
LLVM_PROFILE_FILE="$profileDir/%p-%m.profraw" ctest --test-dir "$buildDir" --output-on-failure -LE "gpu|format"

outDir="$buildDir/coverage"
mkdir -p "$outDir"
"${profdata[@]}" merge -sparse "$profileDir"/*.profraw -o "$outDir/basalt.profdata"

objects=("$buildDir/bin/BasaltTests" -object "$buildDir/bin/basalt")
ignore='(_deps|Tests/|ThirdParty|/build[^/]*/)'
"${cov[@]}" report "${objects[@]}" -instr-profile="$outDir/basalt.profdata" -ignore-filename-regex="$ignore" | tee "$outDir/summary.txt"
"${cov[@]}" show "${objects[@]}" -instr-profile="$outDir/basalt.profdata" -ignore-filename-regex="$ignore" \
	-format=html -output-dir="$outDir/html" -show-line-counts-or-regions
echo "HTML report: $outDir/html/index.html"
