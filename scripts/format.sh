#!/usr/bin/env bash
# Formats every Basalt source file in place with the repository's .clang-format, using $CLANG_FORMAT (default: the
# clang-format on PATH). CI checks with the release pinned in .github/workflows/ci.yml (CLANG_FORMAT_VERSION); when the
# major versions differ, cmake/CheckClangFormatVersion.cmake warns, and formatting still goes ahead. The directories
# match cmake/CheckFormat.cmake's; change both together.
set -euo pipefail
cd "$(dirname "$0")/.."
clangFormat="${CLANG_FORMAT:-clang-format}"

if ! command -v "$clangFormat" > /dev/null 2>&1; then
	echo "error: $clangFormat not found: install clang-format (CI pins CLANG_FORMAT_VERSION in .github/workflows/ci.yml)" \
		"or set CLANG_FORMAT to its path" >&2
	exit 1
fi
if command -v cmake > /dev/null 2>&1; then
	cmake "-DCLANG_FORMAT=$clangFormat" -P cmake/CheckClangFormatVersion.cmake
else
	echo "warning: cmake not found, so $clangFormat's version is not compared with CI's" >&2
fi

find Basalt/Source Basalt-Editor/Source Basalt-Runtime/Source Basalt-CLI/Source Tests/Source Tests/Fuzz Tests/Rendering Tests/Benchmarks \
	\( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 "$clangFormat" -i
echo "Formatted."
