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

for jobs in 0 abc -2 1.5; do
	if output="$(FUZZ_JOBS="$jobs" bash "$script" 10 FuzzGltf 2>&1)"; then
		fail "FUZZ_JOBS '$jobs' was accepted"
	fi
	[[ "$output" == *"FUZZ_JOBS must be a positive integer"* ]] || fail "FUZZ_JOBS '$jobs' printed '$output'"
done

# Running targets side by side, in a copy of the script with stand-ins for cmake, brew and the fuzz targets. FuzzAlpha
# and FuzzBeta each wait for the other to start, so they pass only when they run at the same time.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/scripts" "$work/Tests/Fuzz" "$work/tools" "$work/llvm/bin" "$work/build-fuzz/bin"
cp "$script" "$work/scripts/fuzz.sh"
printf '#!/bin/sh\nexit 0\n' > "$work/tools/cmake"
printf '#!/bin/sh\necho "%s"\n' "$work/llvm" > "$work/tools/brew"
printf '#!/bin/sh\nexit 0\n' > "$work/llvm/bin/clang++"
cat > "$work/fake-fuzzer" << 'EOF_FUZZER'
#!/usr/bin/env bash
name="$(basename "$0")"
for argument in "$@"; do
	[[ "$argument" == -artifact_prefix=* ]] && crashes="${argument#-artifact_prefix=}"
	[[ "$argument" == -rss_limit_mb=* ]] && rss="${argument#-rss_limit_mb=}"
done
touch "$FAKE_STATE/$name.started"
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
	FuzzAlpha) wait_for FuzzBeta ;;
	FuzzBeta) wait_for FuzzAlpha ;;
	FuzzGamma)
		if [[ -n "${FAKE_CRASH:-}" ]]; then
			echo "==1== ERROR: AddressSanitizer: planted"
			echo planted > "${crashes}crash-1"
			exit 1
		fi ;;
esac
echo "stat::peak_rss_mb: limit $rss"
echo "#100 DONE cov: 1"
EOF_FUZZER
for target in FuzzAlpha FuzzBeta FuzzGamma; do
	touch "$work/Tests/Fuzz/$target.cpp"
	cp "$work/fake-fuzzer" "$work/build-fuzz/bin/$target"
done
chmod +x "$work/tools/"* "$work/llvm/bin/clang++" "$work/build-fuzz/bin/"*
export FAKE_STATE="$work"
fuzz()
{
	rm -f "$work"/*.started
	PATH="$work/tools:$PATH" FUZZ_JOBS=2 bash "$work/scripts/fuzz.sh" 5 2>&1
}

output="$(fuzz)" || fail "two targets at a time failed: $output"
for target in FuzzAlpha FuzzBeta FuzzGamma; do
	[[ "$output" == *"=== $target (5s)"*"#100 DONE"* ]] || fail "$target printed no summary: $output"
done
[[ "$output" == *"limit 3072"* && "$output" != *"limit 4096"* ]] || fail "parallel targets kept the single-target memory limit: $output"

if output="$(FAKE_CRASH=1 fuzz)"; then
	fail "a crashing target did not fail the run: $output"
fi
[[ "$output" == *"FuzzGamma: exited with 1"*"AddressSanitizer: planted"* ]] || fail "the crash was not reported: $output"
[[ "$output" == *"=== FuzzAlpha"* && "$output" == *"=== FuzzBeta"* ]] || fail "a crash hid other targets: $output"
# The crash input it left keeps failing the target until it is removed.
if output="$(fuzz)"; then
	fail "a leftover crash input did not fail the run: $output"
fi
[[ "$output" == *"FuzzGamma: exited with 0; crash inputs"* ]] || fail "the leftover crash input was not reported: $output"
echo "fuzz.sh checks passed"
