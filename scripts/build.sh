#!/usr/bin/env bash
# Configures and builds Basalt. Usage: scripts/build.sh [Debug|Release|Dist] (default: Debug)
set -euo pipefail
cd "$(dirname "$0")/.."
config="${1:-Debug}"
buildDir="build"
[[ "$config" != "Debug" ]] && buildDir="build-$(echo "$config" | tr '[:upper:]' '[:lower:]')"
cmake -S . -B "$buildDir" -G Ninja -DCMAKE_BUILD_TYPE="$config"
cmake --build "$buildDir"
