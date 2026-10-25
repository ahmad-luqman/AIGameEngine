#!/usr/bin/env bash
# Fails if a configured build tree does not use CI's caches, which would otherwise only show as a slower build:
# - it downloaded a dependency instead of taking it from BASALT_DEPS_SOURCE_DIR (a stale stamp, a dependency the fetch
#   left out, a configure that does not see the variable; see scripts/ci-fetch-deps.sh);
# - CMAKE_C_COMPILER_LAUNCHER or CMAKE_CXX_COMPILER_LAUNCHER is set in the environment but the tree does not compile
#   through it. Jobs without a compiler cache (release.yml) set neither, and only the first check applies to them.
# Usage: scripts/ci-check-caches.sh <build dir>
set -euo pipefail
shopt -s nullglob

if [[ $# -ne 1 ]]; then
	echo "Usage: $0 <build dir>" >&2
	exit 1
fi
cache="$1/CMakeCache.txt"
if [[ ! -f "$cache" ]]; then
	echo "::error::$1 is not a configured build tree (no CMakeCache.txt)" >&2
	exit 1
fi

downloaded=("$1"/_deps/*-src)
if [[ ${#downloaded[@]} -gt 0 ]]; then
	echo "::error::$1 downloaded ${downloaded[*]##*/} instead of using BASALT_DEPS_SOURCE_DIR (${BASALT_DEPS_SOURCE_DIR:-unset})." \
		"If the restored cache lacks them, delete its deps-src entry (gh cache delete) so the next run fetches again." >&2
	exit 1
fi
echo "$1 downloaded no dependency sources"

for language in C CXX; do
	variable="CMAKE_${language}_COMPILER_LAUNCHER"
	launcher="${!variable:-}"
	[[ -n "$launcher" ]] || continue
	# CMakeCache.txt has CRLF line endings on Windows. No pipe into grep -q: it stops reading at the match, and under
	# pipefail the writer's SIGPIPE would fail the check.
	line="$variable:STRING=$launcher"
	if ! grep -x -F -e "$line" -e "$line"$'\r' "$cache" > /dev/null; then
		echo "::error::$1 does not compile $language through $launcher" >&2
		exit 1
	fi
	echo "$1 compiles $language through $launcher"
done
