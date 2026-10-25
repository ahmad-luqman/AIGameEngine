#!/usr/bin/env bash
# Downloads every pinned dependency (cmake/Dependencies.cmake), optional ones included, into <dir>/<name>-src without
# git history (most of their size), and stamps <dir> with the hash of Dependencies.cmake, which
# cmake/DependencySources.cmake requires before it uses the directory through BASALT_DEPS_SOURCE_DIR.
# Usage: scripts/ci-fetch-deps.sh <dir>
# CI runs it on a dependency-cache miss, before the build's own configure, so cache hits and misses compile the same
# paths. Locally, one fetch into a shared directory lets several worktrees use the same sources.
set -euo pipefail
shopt -s nullglob

if [[ $# -ne 1 ]]; then
	echo "Usage: $0 <dir>" >&2
	exit 1
fi
root="$(cd "$(dirname "$0")/.." && pwd)"

# CMake on Windows needs Windows paths; Git Bash's are POSIX ones.
nativePath() {
	if command -v cygpath > /dev/null; then
		cygpath -m "$1"
	else
		echo "$1"
	fi
}

# Into an empty directory only: sources left from another fetch would end up under the new stamp.
mkdir -p "$1"
if [[ -n "$(ls -A "$1")" ]]; then
	echo "$1 is not empty" >&2
	exit 1
fi
sourceDir="$(nativePath "$(cd "$1" && pwd)")"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# BASALT_PROFILE adds Tracy, and the editor (on by default) adds nativefiledialog where it builds (on Linux only with the
# D-Bus development files), so one fetch serves every job on this OS. BASALT_DEPS_SOURCE_DIR is unset: the directory
# has no stamp until this finishes, so the configure would only warn that it ignores it.
if ! env -u BASALT_DEPS_SOURCE_DIR cmake -S "$(nativePath "$root")" -B "$(nativePath "$scratch")" -G Ninja \
	-DFETCHCONTENT_BASE_DIR="$sourceDir" -DBASALT_PROFILE=ON -DBASALT_CHECK_FORMAT=OFF > "$scratch/configure.log" 2>&1; then
	cat "$scratch/configure.log" >&2
	exit 1
fi
# Every job on this OS restores what this saves, so an optional dependency left out would be downloaded by every job
# that needs it, on every run.
for optional in tracy nativefiledialog; do
	if [[ ! -d "$1/$optional-src" ]]; then
		echo "::error::The fetch left out $optional (missing build prerequisites?); configure log:" >&2
		cat "$scratch/configure.log" >&2
		exit 1
	fi
done

# cmake -E rm also deletes git's read-only objects on Windows.
leftovers=("$1"/*-build "$1"/*-subbuild "$1"/*-src/.git)
if [[ ${#leftovers[@]} -gt 0 ]]; then
	cmake -E rm -rf "${leftovers[@]}"
fi
cmake -E sha256sum "$(nativePath "$root/cmake/Dependencies.cmake")" | cut -d ' ' -f 1 > "$1/pins.sha256"
sources=("$1"/*-src)
echo "Fetched ${#sources[@]} dependency sources into $1 ($(du -sh "$1" | cut -f 1) on disk)"
