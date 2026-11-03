#!/usr/bin/env bash
# Builds the selected libFuzzer targets (all by default; Clang + ASan + UBSan) into build-fuzz/ and runs each one.
# Usage: scripts/fuzz.sh [seconds per target, default 60] [target name filter]
#        scripts/fuzz.sh --list   (prints the target names as a JSON array, for the nightly CI matrix)
# FUZZ_JOBS=N runs N targets at a time (default 1; CI uses one per core); each prints a status line as it finishes, and
# the summaries follow in target order.
# FUZZ_MODE=build only builds the selected targets, and FUZZ_MODE=run only runs them from build-fuzz/bin without
# configuring or compiling anything (stale binaries are not rebuilt); the nightly workflow builds once and runs each
# target on its own runner. The default, build-and-run, does both. Every argument is checked in every mode.
# Seeds come from Tests/Fuzz/Corpus/<Target>/; new interesting inputs collect in build-fuzz/corpus/ and
# crashes in build-fuzz/crashes/ (add a regression test before fixing one). On macOS this needs Homebrew
# LLVM (Apple's clang ships no libFuzzer).
set -euo pipefail
cd "$(dirname "$0")/.."

# Every Tests/Fuzz/Fuzz*.cpp is a target; Tests/Fuzz/CMakeLists.txt globs the same files. The nightly workflow puts the
# names into JSON, cache keys (matched by prefix) and artifact names, so they are letters and digits only.
targets=()
for source in Tests/Fuzz/Fuzz*.cpp; do
	[[ -f "$source" ]] || continue
	target="$(basename "$source" .cpp)"
	if ! [[ "$target" =~ ^Fuzz[A-Za-z0-9]+$ ]]; then
		echo "Fuzz target names are Fuzz followed by letters and digits, not '$target' ($source)" >&2
		exit 1
	fi
	targets+=("$target")
done
if [[ ${#targets[@]} -eq 0 ]]; then
	echo "No fuzz targets in Tests/Fuzz/" >&2
	exit 1
fi
if [[ "${1:-}" == "--list" ]]; then
	if [[ $# -ne 1 ]]; then
		echo "--list takes no other arguments" >&2
		exit 1
	fi
	list="$(printf '"%s",' "${targets[@]}")"
	echo "[${list%,}]"
	exit 0
fi

# An empty argument would otherwise mean the default (60 seconds, every target).
if [[ $# -gt 2 ]] || [[ $# -ge 1 && -z "$1" ]] || [[ $# -eq 2 && -z "$2" ]]; then
	echo "Usage: scripts/fuzz.sh [seconds per target] [target name filter] (one filter at most, neither empty)" >&2
	exit 1
fi
seconds="${1:-60}"
filter="${2:-}"
# libFuzzer keeps only a value's leading digits (so 'abc' reads as 0) and runs without a time limit for 0.
if ! [[ "$seconds" =~ ^[1-9][0-9]*$ ]]; then
	echo "Seconds per target must be a positive integer, not '$seconds'" >&2
	exit 1
fi
jobs="${FUZZ_JOBS:-1}"
if ! [[ "$jobs" =~ ^[1-9][0-9]*$ ]]; then
	echo "FUZZ_JOBS must be a positive integer, not '$jobs'" >&2
	exit 1
fi
mode="${FUZZ_MODE:-build-and-run}"
if ! [[ "$mode" =~ ^(build|run|build-and-run)$ ]]; then
	echo "FUZZ_MODE must be build, run or build-and-run, not '$mode'" >&2
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

if [[ "$mode" == run ]]; then
	# Checked up front: a missing target would otherwise fail only after the others had fuzzed for their full time.
	missing=()
	for target in "${selected[@]}"; do
		[[ -f "$buildDir/bin/$target" && -x "$buildDir/bin/$target" ]] || missing+=("$target")
	done
	if [[ ${#missing[@]} -gt 0 ]]; then
		echo "FUZZ_MODE=run needs built targets, but $buildDir/bin lacks (or cannot execute) ${missing[*]}" >&2
		exit 1
	fi
else
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
	if [[ "$mode" == build ]]; then
		echo "Built ${#selected[@]} targets in $buildDir/bin"
		exit 0
	fi
fi

# Several targets share the machine's memory, so each gets a smaller limit, sized for one target per core on CI's
# 4-core, 16 GB runners (each summary shows the target's peak RSS). An input that makes a target exceed it is an
# out-of-memory finding; libFuzzer's -malloc_limit_mb defaults to the same limit, so one oversized allocation is too.
rssLimit=4096
[[ $jobs -gt 1 ]] && rssLimit=3072

# Runs one target (arguments: target, build dir, seconds, RSS limit; from the repository root, with the sanitizer options
# exported), writes its summary to logs/<target>.summary and prints one status line. Returns 1 for any failure (a
# finding, a kill, a run that did not fuzz), never more: xargs stops starting targets after an exit of 255.
run_target()
{
	# A bash -c child does not inherit the caller's options; -e stays off because exit codes are collected explicitly.
	set -u
	local target="$1" buildDir="$2" seconds="$3" rssLimit="$4"
	local corpus="$buildDir/corpus/$target"
	local crashes="$buildDir/crashes/$target"
	local log="$buildDir/logs/$target.log"
	mkdir -p "$corpus" "$crashes" "$buildDir/logs"
	# The seed directory is read-only input; new findings go into the build-local corpus. A new target may have no
	# seeds yet, and libFuzzer exits on a missing directory.
	local seeds=()
	[[ -d "Tests/Fuzz/Corpus/$target" ]] && seeds=("Tests/Fuzz/Corpus/$target")
	local result=0
	"$buildDir/bin/$target" "$corpus" ${seeds[@]+"${seeds[@]}"} -max_total_time="$seconds" -timeout=10 \
		-rss_limit_mb="$rssLimit" -artifact_prefix="$crashes/" -print_final_stats=1 > "$log" 2>&1 || result=$?
	local summary="=== $target (${seconds}s)"
	summary+=$'\n'"$(grep -E "^(#[0-9]+[[:space:]]+DONE|stat::number_of_executed_units|stat::peak_rss_mb|==[0-9]+== ?ERROR|SUMMARY|artifact_prefix|Test unit written)|runtime error|^ +#[0-9] " "$log")"
	# libFuzzer exits non-zero for every finding and for failures that write no input (a target that cannot start, a
	# missing corpus directory). The crash directory also counts (crash, timeout, out-of-memory and slow-unit inputs),
	# so inputs left by an earlier run keep failing until they are turned into tests and removed. A clean exit without
	# libFuzzer's final DONE line did not fuzz.
	local problem=""
	if [[ $result -gt 128 ]]; then
		problem="exited with $result, probably killed by signal $((result - 128))"
		[[ $result -eq 137 ]] && problem="killed by SIGKILL, likely the kernel's out-of-memory killer"
	elif [[ $result -ne 0 ]]; then
		problem="exited with $result"
	elif compgen -G "$crashes/*" > /dev/null; then
		problem="left crash inputs"
	elif ! grep -q -E "^#[0-9]+[[:space:]]+DONE" "$log"; then
		problem="finished without libFuzzer's DONE line"
	fi
	if [[ -n "$problem" ]]; then
		summary+=$'\n'"$target: $problem; crash inputs (if any) in $crashes, full log in $log"
		summary+=$'\n'"$(tail -n 20 "$log")"
	fi
	printf '%s\n' "$summary" > "$buildDir/logs/$target.summary"
	if [[ -n "$problem" ]]; then
		echo "$target: FAILED ($problem)"
		return 1
	fi
	echo "$target: passed"
}

# handle_sigtrap: a breakpoint trap (e.g. a dependency's debug assert) is reported with a stack trace and its input saved,
# instead of killing the target silently.
export ASAN_OPTIONS="detect_leaks=0:allocator_may_return_null=1:handle_sigtrap=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
export -f run_target
for target in "${selected[@]}"; do
	rm -f "$buildDir/logs/$target.summary"
done
echo "Fuzzing ${#selected[@]} targets for ${seconds}s each, $jobs at a time"
# xargs -P because bash 3.2 (macOS's) has no wait -n. It exits non-zero when any target failed. Targets run in this same
# bash ($BASH), which reads the exported function back. Concurrent output would interleave, so each target prints one
# line as it finishes and the summaries follow in target order.
status=0
printf '%s\n' "${selected[@]}" | xargs -P "$jobs" -I {} "$BASH" -c 'run_target "$@"' run_target {} "$buildDir" "$seconds" \
	"$rssLimit" || status=1
for target in "${selected[@]}"; do
	if [[ -f "$buildDir/logs/$target.summary" ]]; then
		cat "$buildDir/logs/$target.summary"
	else
		# Its bash died before writing one, or xargs stopped before starting it.
		echo "=== $target did not finish; log (if any): $buildDir/logs/$target.log" >&2
		status=1
	fi
done
exit $status
