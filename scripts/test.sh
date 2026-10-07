#!/usr/bin/env bash
# Builds (Debug) and runs every test: unit tests, format check, feature test, samples and (outside CI)
# the GPU rendering test.
set -euo pipefail
cd "$(dirname "$0")/.."
scripts/build.sh Debug
ctest --test-dir build --output-on-failure "$@"
