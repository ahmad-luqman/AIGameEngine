#!/usr/bin/env bash
# Builds (Debug) and runs every test: unit tests, the feature test scene and the format check.
set -euo pipefail
cd "$(dirname "$0")/.."
scripts/build.sh Debug
ctest --test-dir build --output-on-failure "$@"
