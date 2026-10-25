#!/usr/bin/env bash
# Helpers for CI's caches: dependency sources (read by cmake/DependencySources.cmake through BASALT_DEPS_SOURCE_DIR)
# and the compiler cache (CMAKE_<LANG>_COMPILER_LAUNCHER).
# Usage: scripts/ci-cache.sh fetch-deps <dir>   downloads every pinned dependency, optional ones included, into
#                                               <dir>/<name>-src without git history (most of their size), and stamps
#                                               <dir> with the pins it matches. CI runs it on a cache miss before the
#                                               build's own configure, so hits and misses compile the same paths.
#        scripts/ci-cache.sh check <build dir>  fails if that configured tree downloaded a dependency instead of using
#                                               BASALT_DEPS_SOURCE_DIR (a stale stamp, a renamed dependency, a configure
#                                               that does not see the variable), or does not compile through the
#                                               CMAKE_CXX_COMPILER_LAUNCHER that the environment names. Either would
#                                               otherwise only show as a slower build.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"

# CMake on Windows needs Windows paths; Git Bash's are POSIX ones.
nativePath() {
	if command -v cygpath > /dev/null; then
		cygpath -m "$1"
	else
		echo "$1"
	fi
}

case "${1:-}:$#" in
fetch-deps:2)
	mkdir -p "$2"
	sourceDir="$(nativePath "$(cd "$2" && pwd)")"
	scratch="$(mktemp -d)"
	trap 'rm -rf "$scratch"' EXIT
	# BASALT_PROFILE adds Tracy and the editor (on by default) nativefiledialog, so one fetch serves every job on this OS.
	# The variable is unset because the directory has no stamp yet.
	if ! env -u BASALT_DEPS_SOURCE_DIR cmake -S "$(nativePath "$root")" -B "$(nativePath "$scratch")" -G Ninja \
		-DFETCHCONTENT_BASE_DIR="$sourceDir" -DBASALT_PROFILE=ON -DBASALT_CHECK_FORMAT=OFF > "$scratch/configure.log" 2>&1; then
		cat "$scratch/configure.log" >&2
		exit 1
	fi
	# cmake -E rm also deletes git's read-only objects on Windows.
	cmake -E rm -rf "$2"/*-build "$2"/*-subbuild "$2"/*-src/.git
	cmake -E sha256sum "$(nativePath "$root/cmake/Dependencies.cmake")" | cut -d ' ' -f 1 > "$2/pins.sha256"
	echo "Fetched $(ls -d "$2"/*-src | wc -l | tr -d ' ') dependency sources into $2 ($(du -sh "$2" | cut -f 1))"
	;;
check:2)
	downloaded=()
	for source in "$2"/_deps/*-src; do
		if [[ -d "$source" ]]; then downloaded+=("$(basename "$source")"); fi
	done
	if [[ ${#downloaded[@]} -gt 0 ]]; then
		echo "::error::$2 downloaded ${downloaded[*]} instead of using BASALT_DEPS_SOURCE_DIR (${BASALT_DEPS_SOURCE_DIR:-unset})" >&2
		exit 1
	fi
	echo "$2 downloaded no dependency sources"
	if [[ -n "${CMAKE_CXX_COMPILER_LAUNCHER:-}" ]]; then
		# CMakeCache.txt has CRLF line endings on Windows.
		if ! tr -d '\r' < "$2/CMakeCache.txt" | grep -q -x -F "CMAKE_CXX_COMPILER_LAUNCHER:STRING=$CMAKE_CXX_COMPILER_LAUNCHER"; then
			echo "::error::$2 does not compile through $CMAKE_CXX_COMPILER_LAUNCHER" >&2
			exit 1
		fi
		echo "$2 compiles through $CMAKE_CXX_COMPILER_LAUNCHER"
	fi
	;;
*)
	echo "Usage: $0 fetch-deps <dir> | check <build dir>" >&2
	exit 1
	;;
esac
