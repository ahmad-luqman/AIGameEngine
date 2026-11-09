#!/usr/bin/env bash
# Puts universal (arm64 + x86_64) builds of the Vulkan loader and MoltenVK into <dir>/lib, for the macOS release: point
# VULKAN_SDK at <dir> and scripts/package_release.py and the exporter take them from there (Homebrew ships each machine's
# architecture only, and no Intel bottles any more).
# Usage: scripts/ci-universal-vulkan.sh <dir>
#
# MoltenVK comes from its pinned, checksummed GitHub release, which is universal. The loader is built from source at the
# Vulkan-Headers tag that cmake/Dependencies.cmake pins (Khronos tags the loader with the same versions), for both
# architectures and the macOS version the engine targets (CMAKE_OSX_DEPLOYMENT_TARGET in CMakeLists.txt).
set -euo pipefail

if [[ $# -ne 1 ]]; then
	echo "Usage: $0 <dir>" >&2
	exit 2
fi
root="$(cd "$(dirname "$0")/.." && pwd)"
moltenVKVersion="v1.4.2"
moltenVKSha256="f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e"

# The first GIT_TAG after FetchContent_Declare(VulkanHeaders, and the deployment target's default.
headersTag="$(awk '/FetchContent_Declare\(VulkanHeaders/ { found = 1 } found && $1 == "GIT_TAG" { print $2; exit }' "$root/cmake/Dependencies.cmake")"
deploymentTarget="$(sed -n 's/^[[:space:]]*set(CMAKE_OSX_DEPLOYMENT_TARGET "\([0-9.]*\)".*/\1/p' "$root/CMakeLists.txt" | head -n 1)"
if [[ -z "$headersTag" || -z "$deploymentTarget" ]]; then
	echo "cannot read the Vulkan-Headers pin from cmake/Dependencies.cmake or the deployment target from CMakeLists.txt" >&2
	exit 1
fi

mkdir -p "$1/lib"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

curl -fsSL --retry 3 -o "$work/MoltenVK-macos.tar" "https://github.com/KhronosGroup/MoltenVK/releases/download/$moltenVKVersion/MoltenVK-macos.tar"
echo "$moltenVKSha256  $work/MoltenVK-macos.tar" | shasum -a 256 -c -
tar -xf "$work/MoltenVK-macos.tar" -C "$work" MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib
cp "$work/MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" "$1/lib/libMoltenVK.dylib"

for repository in Vulkan-Headers Vulkan-Loader; do
	git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$headersTag" "https://github.com/KhronosGroup/$repository.git" "$work/$repository"
done
cmake -S "$work/Vulkan-Headers" -B "$work/headers-build" -G Ninja -DCMAKE_INSTALL_PREFIX="$work/headers" > /dev/null
cmake --install "$work/headers-build" > /dev/null
cmake -S "$work/Vulkan-Loader" -B "$work/loader-build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF \
	-DCMAKE_PREFIX_PATH="$work/headers" -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET="$deploymentTarget" > /dev/null
cmake --build "$work/loader-build" > /dev/null
# libvulkan.1.dylib is a link to the versioned file; ship the file itself under the name the engine loads.
cp "$(cd "$work/loader-build/loader" && pwd -P)/$(readlink "$work/loader-build/loader/libvulkan.1.dylib" || echo libvulkan.1.dylib)" "$1/lib/libvulkan.1.dylib"

for library in libvulkan.1.dylib libMoltenVK.dylib; do
	architectures="$(lipo -archs "$1/lib/$library")"
	if [[ " $architectures " != *" arm64 "* || " $architectures " != *" x86_64 "* ]]; then
		echo "$library holds '$architectures', not arm64 and x86_64" >&2
		exit 1
	fi
	echo "$1/lib/$library: $architectures"
done
