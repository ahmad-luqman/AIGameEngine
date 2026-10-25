#!/usr/bin/env bash
# Copies a build tree's downloaded dependency sources into CI's dependency cache directory, without their git history
# (most of their size). Later jobs restore that directory as BASALT_DEPS_SOURCE_DIR (cmake/Dependencies.cmake) and skip
# the downloads. Usage: scripts/ci-collect-deps.sh <build dir> <cache dir>
set -euo pipefail

if [[ $# -ne 2 ]]; then
	echo "Usage: $0 <build dir> <cache dir>" >&2
	exit 1
fi
buildDir="$1"
cacheDir="$2"

mkdir -p "$cacheDir"
copied=0
for source in "$buildDir"/_deps/*-src; do
	[[ -d "$source" ]] || continue
	target="$cacheDir/$(basename "$source")"
	[[ -e "$target" ]] && continue
	cp -R "$source" "$target"
	rm -rf "$target/.git"
	copied=$((copied + 1))
done
# A configure that downloaded nothing means the build did not use the cache directory's layout: fail instead of saving
# an empty cache that every later job would restore.
if [[ $copied -eq 0 && -z "$(ls -A "$cacheDir")" ]]; then
	echo "No dependency sources in $buildDir/_deps" >&2
	exit 1
fi
echo "Copied $copied dependency sources into $cacheDir ($(du -sh "$cacheDir" | cut -f1))"
