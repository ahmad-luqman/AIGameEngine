---
name: basalt-build-test
description: Build Basalt, run its unit tests and format check, and verify rendering with Vulkan validation. Use before every commit and after any C++/shader/CMake change.
---

# Build and test Basalt

1. Format: `scripts/format.sh`
2. Build + all tests: `scripts/test.sh` (Debug; wraps `cmake` + `ctest --output-on-failure`).
   - Filter unit tests: `./build/bin/BasaltTests -tc="*Scene*"`; list them: `--list-test-cases`.
3. If the change touches rendering, the editor, or the runtime, launch it with validation:
   ```bash
   DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib ./build/bin/BasaltRuntime   # macOS/Homebrew
   ```
   The log must contain `validation enabled` and no `[Vulkan]` / `[nvrhi]` error lines.
4. Release build sanity check for larger changes: `scripts/build.sh Release`.
5. Test groups are labelled: `ctest --test-dir build -L unit|integration|gpu|format` (or `-LE` to exclude).

Extra checks (each one is also a CI job; run locally when touching memory handling or before big commits):
- Sanitizers: `cmake -S . -B build-asan -G Ninja -DBASALT_SANITIZE="address;undefined" -DBASALT_CHECK_FORMAT=OFF`,
  build, then `ctest --test-dir build-asan -LE "gpu|format"`. Fix reports in Basalt code; only third-party
  reports go into `cmake/sanitizers/*.supp`.
- clang-tidy: configure with `-DBASALT_CLANG_TIDY=ON` (checks in `.clang-tidy`); findings fail the build.
  Fix them, or `NOLINT(check-name)` with a reason when the check is wrong.
- Coverage: `scripts/coverage.sh` (Clang) prints a per-file summary and writes
  `build-coverage/coverage/html/index.html`. Use it to find untested code; it is not a gate.

Failure handling:
- A compiler warning is an error — fix the code, never silence the warning.
- A failing test is never "flaky" by default: find the cause. Tests are deterministic by design.
- If CMake cannot fetch a dependency, check network access; never replace a pinned dependency with an unpinned one.
