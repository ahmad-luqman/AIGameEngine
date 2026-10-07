#!/usr/bin/env bash
# Re-renders the feature scene's golden images and copies them into Tests/Data/Golden/<driver>/.
# Usage: scripts/update_golden.sh [driver]   (default: the build's BASALT_GOLDEN_DRIVER, e.g. moltenvk)
# Look at every new image (and the diff images under build/Tests/Golden) before committing them:
# a reference is only as good as the render it was taken from.
set -euo pipefail
cd "$(dirname "$0")/.."
buildDir="build"

driver="${1:-}"
cmake -S . -B "$buildDir" -G Ninja -DBASALT_GPU_TESTS=ON ${driver:+-DBASALT_GOLDEN_DRIVER="$driver"} > /dev/null
driver="$(sed -n 's/^BASALT_GOLDEN_DRIVER:STRING=//p' "$buildDir/CMakeCache.txt")"
if [[ -z "$driver" ]]; then
	echo "No golden driver set: pass one, e.g. scripts/update_golden.sh lavapipe" >&2
	exit 1
fi
cmake --build "$buildDir"

if [[ "$(uname)" == "Darwin" ]]; then
	export DYLD_FALLBACK_LIBRARY_PATH="${DYLD_FALLBACK_LIBRARY_PATH:-/opt/homebrew/lib:/usr/local/lib}"
fi
# Only the renders: the comparisons would fail against the old references.
ctest --test-dir "$buildDir" --output-on-failure -R '^FeatureTestRender(_[A-Za-z]+)?$'

target="Tests/Data/Golden/$driver"
mkdir -p "$target"
for view in Lit SSAO Normals Depth; do
	cp "$buildDir/Tests/Golden/FeatureTest_$view.png" "$target/"
done
echo "Updated $target. Review the images, then commit them."
