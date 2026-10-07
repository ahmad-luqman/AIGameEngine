#!/usr/bin/env bash
# Formats every Basalt source file in place with the repository's .clang-format.
set -euo pipefail
cd "$(dirname "$0")/.."
find Basalt/Source Basalt-Editor/Source Basalt-Runtime/Source Basalt-CLI/Source Tests/Source Tests/Fuzz \
	\( -name '*.cpp' -o -name '*.h' \) -print0 2>/dev/null | xargs -0 clang-format -i
echo "Formatted."
