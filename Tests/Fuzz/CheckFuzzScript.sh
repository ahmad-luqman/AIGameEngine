#!/usr/bin/env bash
# Checks scripts/fuzz.sh (ctest FuzzScript): the target list the nightly workflow's matrix reads, the rejections that
# keep a bad input from fuzzing forever, and how it runs targets side by side (FUZZ_JOBS) with stand-in targets.
set -euo pipefail
script="$1/scripts/fuzz.sh"
fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

list="$(bash "$script" --list)"
[[ "$list" == \[*'"FuzzGltf"'*\] ]] || fail "--list printed '$list'"

for seconds in 0 abc -5 1.5 2s; do
	if output="$(bash "$script" "$seconds" FuzzGltf 2>&1)"; then
		fail "seconds '$seconds' was accepted"
	fi
	[[ "$output" == *"must be a positive integer"* ]] || fail "seconds '$seconds' printed '$output'"
done

if output="$(bash "$script" 10 NoSuchTarget 2>&1)"; then
	fail "an unknown target was accepted"
fi
[[ "$output" == *"No fuzz target matches 'NoSuchTarget'"* ]] || fail "an unknown target printed '$output'"

# The rest runs a copy of the script with stand-ins for cmake, brew and the fuzz targets, so nothing is built or fuzzed
# here even if a check regresses. FuzzAlpha and FuzzBeta each wait for the other to start, so they pass only when they
# run at the same time, and FuzzAlpha finishes after FuzzBeta, against the target order. FAKE_CRASH, FAKE_KILL,
# FAKE_KILLSHELL and FAKE_NODONE make the named target misbehave; FAKE_BUILD_FAIL fails the build.
unset FUZZ_JOBS
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/scripts" "$work/Tests/Fuzz/Corpus/FuzzAlpha" "$work/tools" "$work/llvm/bin" "$work/build-fuzz/bin"
cp "$script" "$work/scripts/fuzz.sh"
printf '#!/bin/sh\ncase "$*" in *--build*) [ -n "${FAKE_BUILD_FAIL:-}" ] && exit 1 ;; esac\nexit 0\n' > "$work/tools/cmake"
printf '#!/bin/sh\necho "%s"\n' "$work/llvm" > "$work/tools/brew"
printf '#!/bin/sh\nexit 0\n' > "$work/llvm/bin/clang++"
cat > "$work/fake-fuzzer" << 'EOF_FUZZER'
#!/usr/bin/env bash
name="$(basename "$0")"
echo "arguments: $*"
echo "ASAN_OPTIONS=$ASAN_OPTIONS"
for argument in "$@"; do
	[[ "$argument" == -artifact_prefix=* ]] && crashes="${argument#-artifact_prefix=}"
	[[ "$argument" == -rss_limit_mb=* ]] && rss="${argument#-rss_limit_mb=}"
done
touch "$FAKE_STATE/$name.started"
if [[ "${FAKE_CRASH:-}" == "$name" ]]; then
	echo "==1== ERROR: AddressSanitizer: planted"
	echo planted > "${crashes}crash-1"
	exit 1
fi
[[ "${FAKE_KILL:-}" == "$name" ]] && kill -9 $$
# Kills the bash running the target, which then never writes a summary.
if [[ "${FAKE_KILLSHELL:-}" == "$name" ]]; then
	kill -TERM $PPID
	exit 0
fi
wait_for()
{
	for _ in $(seq 100); do
		[[ -e "$FAKE_STATE/$1.started" ]] && return 0
		sleep 0.1
	done
	echo "$1 never started"
	exit 2
}
case "$name" in
	FuzzAlpha) wait_for FuzzBeta && wait_for FuzzBeta.done ;;
	FuzzBeta) wait_for FuzzAlpha ;;
esac
echo "stat::peak_rss_mb: limit $rss"
[[ "${FAKE_NODONE:-}" == "$name" ]] || printf '#100\tDONE   cov: 1 %s\n' "$name"
touch "$FAKE_STATE/$name.done.started"
EOF_FUZZER
for target in FuzzAlpha FuzzBeta FuzzGamma; do
	touch "$work/Tests/Fuzz/$target.cpp"
	cp "$work/fake-fuzzer" "$work/build-fuzz/bin/$target"
done
chmod +x "$work/tools/"* "$work/llvm/bin/clang++" "$work/build-fuzz/bin/"*
export FAKE_STATE="$work"
fuzz()
{
	rm -f "$work"/*.started "$work"/*.done
	(cd "$work" && PATH="$work/tools:$PATH" FUZZ_JOBS="${JOBS:-2}" "${SHELL_UNDER_TEST:-bash}" scripts/fuzz.sh 5 "$@" 2>&1)
}
# Each summary block is whole and they come in target order: every DONE line names the target of the block it is in.
check_blocks()
{
	local blocks
	blocks="$(awk '/^=== /{ name = $2; order = order " " name } /\tDONE/{ if ($NF != name) bad = 1 } END { print (bad ? "mixed" : order) }' <<< "$1")"
	[[ "$blocks" == " FuzzAlpha FuzzBeta FuzzGamma" ]] || fail "summaries are not whole blocks in target order ($blocks): $1"
}

for jobs in 0 abc -2 1.5; do
	if output="$(JOBS="$jobs" fuzz FuzzGamma)"; then
		fail "FUZZ_JOBS '$jobs' was accepted"
	fi
	[[ "$output" == *"FUZZ_JOBS must be a positive integer"* ]] || fail "FUZZ_JOBS '$jobs' printed '$output'"
done

shells=(bash)
# macOS's own bash is 3.2, which the script must support (no wait -n); bash on PATH is often a newer Homebrew one.
[[ "$(uname)" == "Darwin" && -x /bin/bash ]] && shells+=(/bin/bash)
for shell in "${shells[@]}"; do
	output="$(SHELL_UNDER_TEST="$shell" fuzz)" || fail "two targets at a time failed under $shell: $output"
	for target in FuzzAlpha FuzzBeta FuzzGamma; do
		[[ "$output" == *"$target: passed"* ]] || fail "$target did not pass under $shell: $output"
	done
	check_blocks "$output"
done
[[ "$output" == *"limit 3072"* && "$output" != *"limit 4096"* ]] || fail "parallel targets kept the single-target memory limit: $output"
log="$(cat "$work/build-fuzz/logs/FuzzAlpha.log")"
[[ "$log" == *"arguments: build-fuzz/corpus/FuzzAlpha Tests/Fuzz/Corpus/FuzzAlpha -max_total_time=5 "* ]] ||
	fail "FuzzAlpha did not get its corpus, seeds and time limit: $log"
[[ "$log" == *"ASAN_OPTIONS=detect_leaks=0"* ]] || fail "the sanitizer options were not passed on: $log"
# A target without seeds gets none (libFuzzer exits on a missing directory).
log="$(cat "$work/build-fuzz/logs/FuzzBeta.log")"
[[ "$log" == *"arguments: build-fuzz/corpus/FuzzBeta -max_total_time=5 -timeout=10 -rss_limit_mb=3072 -artifact_prefix=build-fuzz/crashes/FuzzBeta/ -print_final_stats=1"$'\n'* ]] ||
	fail "FuzzBeta did not get exactly its corpus and options: $log"

# One target at a time by default (the nightly runs), with the full memory limit.
output="$(cd "$work" && PATH="$work/tools:$PATH" bash scripts/fuzz.sh 5 FuzzGamma 2>&1)" || fail "a single target failed: $output"
[[ "$output" == *"1 at a time"* && "$output" == *"limit 4096"* ]] || fail "one target at a time did not get 4096 MB: $output"

# A target failing first does not stop or hide the others.
if output="$(FAKE_CRASH=FuzzAlpha fuzz)"; then
	fail "a crashing target did not fail the run: $output"
fi
[[ "$output" == *"FuzzAlpha: FAILED (exited with 1)"*"AddressSanitizer: planted"* ]] || fail "the crash was not reported: $output"
[[ "$output" == *"FuzzBeta: passed"* && "$output" == *"FuzzGamma: passed"* ]] || fail "a crash stopped other targets: $output"
check_blocks "$output"
# The crash input it left keeps failing the target until it is removed.
if output="$(fuzz)"; then
	fail "a leftover crash input did not fail the run: $output"
fi
[[ "$output" == *"FuzzAlpha: FAILED (left crash inputs)"* ]] || fail "the leftover crash input was not reported: $output"
rm -f "$work/build-fuzz/crashes/FuzzAlpha/"*

if output="$(FAKE_KILL=FuzzGamma fuzz)"; then
	fail "a killed target did not fail the run: $output"
fi
[[ "$output" == *"FuzzGamma: FAILED (killed by SIGKILL, likely the kernel's out-of-memory killer)"* ]] ||
	fail "the kill was not reported: $output"
if output="$(FAKE_NODONE=FuzzGamma fuzz)"; then
	fail "a target that did not fuzz passed: $output"
fi
[[ "$output" == *"FuzzGamma: finished without libFuzzer's DONE line; "*"arguments: build-fuzz/corpus/FuzzGamma"* ]] ||
	fail "the missing DONE line was not reported with the log's end: $output"

# A target whose bash dies writes no summary: it is reported, and a summary left by an earlier run is not shown instead.
printf '=== FuzzGamma (5s)\nSTALE\n' > "$work/build-fuzz/logs/FuzzGamma.summary"
if output="$(FAKE_KILLSHELL=FuzzGamma fuzz FuzzGamma)"; then
	fail "a target whose bash died passed: $output"
fi
[[ "$output" == *"=== FuzzGamma did not finish; log (if any): build-fuzz/logs/FuzzGamma.log"* && "$output" != *STALE* ]] ||
	fail "a target that did not finish was not reported, or a stale summary was shown: $output"
# A failed build fuzzes nothing.
if output="$(FAKE_BUILD_FAIL=1 fuzz)"; then
	fail "a failed build passed: $output"
fi
compgen -G "$work/*.started" > /dev/null && fail "targets ran after a failed build: $output"
echo "fuzz.sh checks passed"
