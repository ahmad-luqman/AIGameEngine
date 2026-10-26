#!/usr/bin/env bash
# Builds (Debug) and runs every test: unit tests, format check, feature test, samples and (outside CI)
# the GPU rendering test. Tests run side by side, one per core (CTEST_PARALLEL_LEVEL overrides; so does passing -j N,
# since ctest takes the last -j).
set -euo pipefail
cd "$(dirname "$0")/.."
scripts/build.sh Debug
ctest --test-dir build --output-on-failure --no-tests=error -j "${CTEST_PARALLEL_LEVEL:-$(getconf _NPROCESSORS_ONLN)}" "$@"
