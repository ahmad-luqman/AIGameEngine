#!/usr/bin/env bash
# Configures and builds Basalt. Usage: scripts/build.sh [Debug|Release|Dist] (default: Debug)
# GPU tests are registered unless the CI environment variable is set (CI machines have no Vulkan device).
set -euo pipefail
cd "$(dirname "$0")/.."
config="${1:-Debug}"
buildDir="build"
[[ "$config" != "Debug" ]] && buildDir="build-$(echo "$config" | tr '[:upper:]' '[:lower:]')"
gpuTests=ON
[[ -n "${CI:-}" ]] && gpuTests=OFF
cmake -S . -B "$buildDir" -G Ninja -DCMAKE_BUILD_TYPE="$config" -DBASALT_GPU_TESTS="$gpuTests"
cmake --build "$buildDir"
