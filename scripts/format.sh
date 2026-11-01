#!/usr/bin/env bash
# Formats every Basalt source file in place with the repository's .clang-format. CI checks with the clang-format release
# pinned in .github/workflows/ci.yml (CLANG_FORMAT_VERSION); other major versions format some code differently.
set -euo pipefail
cd "$(dirname "$0")/.."
find Basalt/Source Basalt-Editor/Source Basalt-Runtime/Source Basalt-CLI/Source Tests/Source Tests/Fuzz Tests/Rendering Tests/Benchmarks \
	\( -name '*.cpp' -o -name '*.h' \) -print0 2>/dev/null | xargs -0 clang-format -i
echo "Formatted."
