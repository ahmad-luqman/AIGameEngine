#!/usr/bin/env bash
# Formats every Basalt source file in place with the repository's .clang-format, using $CLANG_FORMAT (default: the
# clang-format on PATH). CI checks with the release pinned in .github/workflows/ci.yml (CLANG_FORMAT_VERSION); another
# major version formats some code differently, so this warns (and still formats) when the majors differ.
set -euo pipefail
cd "$(dirname "$0")/.."
clangFormat="${CLANG_FORMAT:-clang-format}"

pinned=$(sed -n 's/^ *CLANG_FORMAT_VERSION: *\([0-9][0-9.]*\) *$/\1/p' .github/workflows/ci.yml | head -n 1)
actual=$("$clangFormat" --version | sed -n 's/.*clang-format version \([0-9][0-9.]*\).*/\1/p' | head -n 1)
if [[ -z "$pinned" ]]; then
	echo "warning: CLANG_FORMAT_VERSION not found in .github/workflows/ci.yml, so the version is not compared" >&2
elif [[ "${actual%%.*}" != "${pinned%%.*}" ]]; then
	echo "warning: $clangFormat is version ${actual:-unknown} but CI checks with $pinned, so the result may differ;" \
		"install that one (python3 -m venv <dir> && <dir>/bin/pip install clang-format==$pinned) and set" \
		"CLANG_FORMAT=<dir>/bin/clang-format" >&2
fi

find Basalt/Source Basalt-Editor/Source Basalt-Runtime/Source Basalt-CLI/Source Tests/Source Tests/Fuzz Tests/Rendering Tests/Benchmarks \
	\( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 "$clangFormat" -i
echo "Formatted."
