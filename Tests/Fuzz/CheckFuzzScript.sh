#!/usr/bin/env bash
# Checks scripts/fuzz.sh's argument handling, which runs before it configures or builds anything (ctest FuzzScript):
# the target list the nightly workflow's matrix reads, and the rejections that keep a bad input from fuzzing forever.
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
echo "fuzz.sh argument checks passed"
