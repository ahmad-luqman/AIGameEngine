#!/usr/bin/env bash
# Puts universal (arm64 + x86_64) builds of the Vulkan loader and MoltenVK into <dir>/lib, for the macOS release: point
# VULKAN_SDK at <dir> and scripts/package_release.py takes them from there, as does the exporter when none sit next to
# the runtime (Homebrew's bottles hold one architecture).
# Usage: scripts/ci-universal-vulkan.sh <dir>
#
# MoltenVK comes from its pinned, checksummed GitHub release, which is universal. The loader is built from source at the
# Vulkan-Headers tag that cmake/Dependencies.cmake pins (Khronos tags the loader with the same versions), for both
# architectures and the macOS version the engine targets (the CMAKE_OSX_DEPLOYMENT_TARGET default in CMakeLists.txt).
# Sources come as GitHub tag archives over HTTPS (no clone): the same tags, and the same trust, as FetchContent's clones of
# the engine's pins.
set -euo pipefail

if [[ $# -ne 1 ]]; then
	echo "Usage: $0 <dir>" >&2
	exit 2
fi
root="$(cd "$(dirname "$0")/.." && pwd)"
lib="$1/lib"
moltenVKVersion="v1.4.2"
moltenVKSha256="f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e"

# The first GIT_TAG after FetchContent_Declare(VulkanHeaders, and the deployment target's default.
headersTag="$(awk '/FetchContent_Declare\(VulkanHeaders/ { found = 1 } found && $1 == "GIT_TAG" { print $2; exit }' "$root/cmake/Dependencies.cmake")"
deploymentTarget="$(sed -n 's/^[[:space:]]*set(CMAKE_OSX_DEPLOYMENT_TARGET "\([0-9.]*\)".*/\1/p' "$root/CMakeLists.txt" | head -n 1)"
if [[ -z "$headersTag" || -z "$deploymentTarget" ]]; then
	echo "cannot read the Vulkan-Headers pin from cmake/Dependencies.cmake or the deployment target from CMakeLists.txt" >&2
	exit 1
fi

mkdir -p "$lib"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Runs a step quietly, printing its whole output (compiler errors go to stdout under Ninja) only when it fails.
quietly() {
	local log="$work/step.log"
	if ! "$@" > "$log" 2>&1; then
		cat "$log" >&2
		echo "failed: $*" >&2
		exit 1
	fi
}

# Any failure is retried (a reset connection failed a clone here once), a missing tag included, which then fails by name.
download() {
	if ! curl -fsSL --retry 5 --retry-all-errors --retry-delay 5 -o "$2" "$1"; then
		echo "cannot download $1" >&2
		exit 1
	fi
}

download "https://github.com/KhronosGroup/MoltenVK/releases/download/$moltenVKVersion/MoltenVK-macos.tar" "$work/MoltenVK-macos.tar"
echo "$moltenVKSha256  $work/MoltenVK-macos.tar" | shasum -a 256 -c -
tar -xf "$work/MoltenVK-macos.tar" -C "$work" MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib
cp "$work/MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" "$lib/libMoltenVK.dylib"

for repository in Vulkan-Headers Vulkan-Loader; do
	download "https://github.com/KhronosGroup/$repository/archive/refs/tags/$headersTag.tar.gz" "$work/$repository.tar.gz"
	mkdir "$work/$repository"
	tar -xzf "$work/$repository.tar.gz" -C "$work/$repository" --strip-components 1
done
quietly cmake -S "$work/Vulkan-Headers" -B "$work/headers-build" -G Ninja -DCMAKE_INSTALL_PREFIX="$work/headers"
quietly cmake --install "$work/headers-build"
quietly cmake -S "$work/Vulkan-Loader" -B "$work/loader-build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF \
	-DCMAKE_PREFIX_PATH="$work/headers" -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET="$deploymentTarget"
quietly cmake --build "$work/loader-build"
# libvulkan.1.dylib is a link to the versioned file; -L ships the file itself under the name the engine loads.
cp -L "$work/loader-build/loader/libvulkan.1.dylib" "$lib/libvulkan.1.dylib"

for library in libvulkan.1.dylib libMoltenVK.dylib; do
	# One architecture per -verify_arch: some lipo versions take no more.
	for architecture in arm64 x86_64; do
		if ! lipo "$lib/$library" -verify_arch "$architecture"; then
			echo "$library holds '$(lipo -archs "$lib/$library")', not arm64 and x86_64" >&2
			exit 1
		fi
	done
	echo "$lib/$library: $(lipo -archs "$lib/$library")"
done
echo "the loader is built for macOS $deploymentTarget"
