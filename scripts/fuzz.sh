#!/usr/bin/env bash
# Builds the selected libFuzzer targets (all by default; Clang + ASan + UBSan) into build-fuzz/ and runs each one.
# Usage: scripts/fuzz.sh [seconds per target, default 60] [target name filter]
#        scripts/fuzz.sh --list   (prints the target names as a JSON array, for the nightly CI matrix)
# Seeds come from Tests/Fuzz/Corpus/<Target>/; new interesting inputs collect in build-fuzz/corpus/ and
# crashes in build-fuzz/crashes/ (add a regression test before fixing one). On macOS this needs Homebrew
# LLVM (Apple's clang ships no libFuzzer).
set -euo pipefail
cd "$(dirname "$0")/.."

# Every Tests/Fuzz/Fuzz*.cpp is a target; Tests/Fuzz/CMakeLists.txt globs the same files.
targets=()
for source in Tests/Fuzz/Fuzz*.cpp; do
	[[ -f "$source" ]] && targets+=("$(basename "$source" .cpp)")
done
if [[ ${#targets[@]} -eq 0 ]]; then
	echo "No fuzz targets in Tests/Fuzz/" >&2
	exit 1
fi
if [[ "${1:-}" == "--list" ]]; then
	list="$(printf '"%s",' "${targets[@]}")"
	echo "[${list%,}]"
	exit 0
fi

seconds="${1:-60}"
filter="${2:-}"
# libFuzzer keeps only a value's leading digits (so 'abc' reads as 0) and runs without a time limit for 0.
if ! [[ "$seconds" =~ ^[1-9][0-9]*$ ]]; then
	echo "Seconds per target must be a positive integer, not '$seconds'" >&2
	exit 1
fi
buildDir="build-fuzz"
# An exact target name selects only that target (the nightly matrix passes one); otherwise the filter is a substring.
selected=()
for target in "${targets[@]}"; do
	[[ "$target" == "$filter" ]] && selected=("$target")
done
if [[ ${#selected[@]} -eq 0 ]]; then
	for target in "${targets[@]}"; do
		[[ -z "$filter" || "$target" == *"$filter"* ]] && selected+=("$target")
	done
fi
if [[ ${#selected[@]} -eq 0 ]]; then
	echo "No fuzz target matches '$filter'" >&2
	exit 1
fi

compiler=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
if [[ "$(uname)" == "Darwin" ]]; then
	llvm="$(brew --prefix llvm 2>/dev/null || true)"
	if [[ -z "$llvm" || ! -x "$llvm/bin/clang++" ]]; then
		echo "Fuzzing on macOS needs Homebrew LLVM: brew install llvm" >&2
		exit 1
	fi
	compiler=(-DCMAKE_C_COMPILER="$llvm/bin/clang" -DCMAKE_CXX_COMPILER="$llvm/bin/clang++")
fi

cmake -S . -B "$buildDir" -G Ninja -DCMAKE_BUILD_TYPE=Debug "${compiler[@]}" \
	-DBASALT_FUZZ=ON -DBASALT_SANITIZE="address;undefined" \
	-DBASALT_BUILD_EDITOR=OFF -DBASALT_BUILD_RUNTIME=OFF -DBASALT_BUILD_TESTS=OFF > /dev/null
cmake --build "$buildDir" --target "${selected[@]}"

export ASAN_OPTIONS="detect_leaks=0:allocator_may_return_null=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
status=0
for target in "${selected[@]}"; do
	corpus="$buildDir/corpus/$target"
	crashes="$buildDir/crashes/$target"
	log="$buildDir/logs/$target.log"
	mkdir -p "$corpus" "$crashes" "$buildDir/logs"
	# The seed directory is read-only input; new findings go into the build-local corpus. A new target may have no
	# seeds yet, and libFuzzer exits on a missing directory.
	seeds=()
	[[ -d "Tests/Fuzz/Corpus/$target" ]] && seeds=("Tests/Fuzz/Corpus/$target")
	echo "=== $target (${seconds}s)"
	result=0
	"$buildDir/bin/$target" "$corpus" ${seeds[@]+"${seeds[@]}"} -max_total_time="$seconds" -timeout=10 -rss_limit_mb=4096 \
		-artifact_prefix="$crashes/" -print_final_stats=1 > "$log" 2>&1 || result=$?
	grep -E "^(#[0-9]+ +DONE|stat::number_of_executed_units|==[0-9]+== ?ERROR|SUMMARY|artifact_prefix|Test unit written)|runtime error|^ +#[0-9] " "$log" || true
	# libFuzzer exits non-zero for every finding and for failures that write no input (a target that cannot start, a
	# missing corpus directory). The crash directory also counts, so inputs left by an earlier run keep failing until
	# they are turned into tests and removed.
	if [[ $result -ne 0 ]] || compgen -G "$crashes/*" > /dev/null; then
		echo "$target: exited with $result; crash inputs (if any) in $crashes, full log in $log" >&2
		[[ $result -ne 0 ]] && tail -n 20 "$log" >&2
		status=1
	fi
done
exit $status
