# CI speed-up plan

A multi-session plan to cut CI wall-clock time and remove duplicated work in `.github/workflows/`. Each session is
one PR (rebase-merged after CI is green). Update the **Status** column and the progress log at the end of each
session, and delete this file when the last session ships.

## Baseline (measured 2026-10-25, runs 40123053488, 40120313607, 40117347130)

A CI run takes 25–36 minutes. Wall time is set by the three longest jobs; everything else only saves runner minutes
(free for this public repository) until those three shrink.

| Job | Time | Where the time goes |
|-----|------|---------------------|
| ubuntu / clang-tidy | 23–35 min | All in Build: ~280 dependency TUs are compiled, then every Basalt TU is compiled *and* tidied |
| ubuntu / GPU tests (lavapipe) | 24–27 min | 11–14 min build, 8–11 min GPU tests run serially (LocalShadowRender 102 s, EditorViewport 89 s, FSR1Render 60 s) |
| ubuntu / fuzz | 20–26 min | ~10 min build, then 12 targets (now 13) × 60 s run one after another |
| windows / Release | 17–25 min | 15–20 min build |
| windows / Debug | 12–19 min | 6–11 min build, 2–4 min tests |
| ubuntu / Debug, ASan+UBSan, coverage | 12–18 min | full build each; ctest serial (267–294 s) |
| ubuntu / Release, macOS Debug/Release | 8–15 min | |

Other workflows: `release.yml` (path-filtered PRs, tags) 10–16 min; `fuzz-nightly.yml` ~30 min, 12 jobs that each
rebuild the same fuzz build.

Findings behind the plan:

1. **The `_deps` cache saves only the download.** `.ninja_log`/`.ninja_deps` live in `build/`, so every Linux job
   recompiles all ~277 dependency TUs even on "Cache hit" (counted in the run logs). The seven per-job keys
   (`deps-plain-*-Debug/Release`, `deps-asan`, `deps-tidy`, `deps-coverage`, `deps-fuzz`, `deps-dist`) hold
   near-identical sources plus useless object files; ubuntu/Release and lavapipe even share one key.
2. **ctest always runs serially** (no `-j`, no `CTEST_PARALLEL_LEVEL`). Locally (16 cores) the headless set went
   91.6 s → 40 s with `-j4` (3/3 green, also green with `-j8`) and the GPU set 124 s → 60 s with `-j4` (2/2 green).
   CI runners have 4 vCPUs (macos-15: 3), so expect less.
3. **`ReleasePackaging` takes 128 s on Linux Debug/ASan but 7–10 s on macOS** — probably `shutil.make_archive`
   gzipping DWARF-heavy Debug binaries at the default level (`--with-engine`), unverified. Once tests run in
   parallel it becomes the longest test.
4. **Duplicate builds.** Linux builds seven full trees per run. ubuntu/Release is a strict subset of lavapipe
   (same Release + benchmark flags; lavapipe adds `BASALT_GPU_TESTS=ON` and `BASALT_GOLDEN_DRIVER=lavapipe`).
   ubuntu/Debug (GCC) runs the same tests as ASan (Clang Debug).
5. **clang-tidy runs inside the compile** (`CXX_CLANG_TIDY` from `basalt_set_warnings`, `cmake/CompilerWarnings.cmake`),
   so the job pays for the full compile of everything, third-party included, and the tidy+compile pair per Basalt TU.
6. **YAML duplication.** The Ubuntu apt package list is repeated 8× across `ci.yml`, `fuzz-nightly.yml` and
   `release.yml`; the Windows trio (Vulkan SDK, `msvc-dev-cmd`, `choco install ninja`) is in both `ci.yml` and
   `release.yml`.

`main` has no branch protection or rulesets (checked 2026-10-25), so renaming or removing jobs blocks nothing.
Re-check with `gh api repos/ahmad-luqman/AIGameEngine/rulesets` before removing a job in case that changed.

Target after sessions 1–3: about 15 minutes per run.

## Sessions

| # | PR | Cuts wall time? | Status |
|---|----|-----------------|--------|
| 1 | Compiler cache, one dependency-source cache, merge ubuntu/Release into lavapipe | Yes (every build) | Merged (#56) |
| 2 | Shard clang-tidy; run fuzz targets concurrently | Yes (two longest jobs) | Merged (#57) |
| 3 | Parallel ctest, fast `ReleasePackaging`, shard `BasaltTests` | Yes (test steps) | Todo |
| 4 | Remove redundant jobs; early format job; coverage on `main` only | Runner minutes, faster feedback | Todo |
| 5 | Shared setup action; nightly fuzz builds once | Maintenance | Todo |
| — | Nightly fuzz findings (FuzzGltf, FuzzCommand) | Separate bug-fix PRs | FuzzCommand in review (#58); FuzzGltf Todo |

### Session 1 — compiler cache and dependency cache

Goal: unchanged TUs (all third-party code, most of Basalt) are never recompiled.

- Add a launcher option, e.g. `cmake/CompilerCache.cmake`: when `BASALT_COMPILER_CACHE` names a program (or
  `ccache`/`sccache` is found and the option is ON) set `CMAKE_C_COMPILER_LAUNCHER`/`CMAKE_CXX_COMPILER_LAUNCHER`
  *before* the dependencies are declared, so FetchContent targets use it too. Keep it OFF by default locally.
- Linux/macOS: `hendrikmuhs/ccache-action` (pin a version) with `key: <job>-<os>-<config>`, restore from `main`'s
  latest entry. Set `CCACHE_BASEDIR=$GITHUB_WORKSPACE`, `CCACHE_COMPILERCHECK=content`, a max size per job (~500 MB),
  and `CCACHE_SLOPPINESS` only if hit rates prove poor. Print `ccache -s` at the end of each job.
- Windows: sccache (or ccache ≥ 4.6) with `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded` (`/Z7`; policy CMP0141,
  fine with CMake ≥ 3.25), otherwise Debug objects with `/Zi` are uncacheable. Check that PDB-dependent tests
  (crash report stack traces, `CrashReport` test) still pass with embedded debug info.
- clang-tidy: `CXX_CLANG_TIDY` bypasses the launcher for the tidy step, but the dependency TUs (no tidy) still hit.
- Replace the per-job `_deps` caches with one per OS holding only sources: set `FETCHCONTENT_BASE_DIR` to a shared
  directory (e.g. `${{ github.workspace }}/.deps`) and cache `.deps/*-src` with key
  `deps-src-${{ runner.os }}-${{ hashFiles('cmake/Dependencies.cmake') }}`. Check that `scripts/build.sh` and local
  builds are unaffected (only CI passes the variable). The GitHub cache limit is 10 GB per repository: sum of
  ccache sizes plus three source caches must fit; watch eviction in the Actions cache page.
- Merge ubuntu/Release into gpu-lavapipe: drop `ubuntu-24.04` × `Release` from the matrix (`exclude:`), and make the
  lavapipe job run `ctest -LE format` after the GPU-registration check (headless + GPU + benchmark), uploading the
  benchmark report as `benchmark-Linux-Release` too. Rename it `ubuntu-24.04 / Release + GPU (lavapipe)`.
- Verify: two consecutive pushes to the PR — the second run's builds should be mostly cache hits (compare Build step
  minutes with the baseline table and record them below).

### Session 2 — the two longest jobs

Goal: clang-tidy and fuzz drop to ~12 minutes or less.

- **clang-tidy shards.** New job matrix `shard: [1, 2, 3]`:
  1. Configure with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` (not `BASALT_CLANG_TIDY`), Clang, Debug.
  2. Build only what tidy needs to parse: the embedded-shader target(s) and any other generated headers (find them
     with `ninja -t targets` / `ninja -t query`; possibly `cmake --build build --target BasaltEmbeddedShaders` or
     similar), not the full tree.
  3. `run-clang-tidy -p build -j "$(nproc)" -quiet "<regex>"` where each shard's regex covers a third of Basalt's
     own sources (e.g. split by module directory so shards stay balanced; `Basalt/Source/Basalt/(Animation|Renderer|…)`).
     Exclude `_deps/` explicitly. `.clang-tidy` already has `WarningsAsErrors: '*'`; make sure the script's exit code
     fails the job (it does when any file fails).
  4. A tiny checker step (or a unit test like `FuzzScript`) that every Basalt `.cpp` matches exactly one shard regex,
     so new modules are never silently skipped.
  Keep `BASALT_CLANG_TIDY=ON` working for local use (memory: local clang-tidy reproduces CI failures).
  Policy question for the user: tidy `BasaltTests` sources too (current behaviour) or only engine/apps.
- **Fuzz in parallel.** `scripts/fuzz.sh`: add a `FUZZ_JOBS` environment variable (default 1) and run that many
  targets concurrently (background processes + `wait`, or `xargs -P`), keeping one log per target and printing each
  summary after its target finishes; the exit status must still be non-zero if any target fails. CI uses
  `FUZZ_JOBS=4`. Mind memory: `-rss_limit_mb=4096` × 4 under ASan on a 16 GB runner — lower to 3072 if runners swap.
  Consider 30 s per target on pushes (the nightly runs 1200 s). Update `Tests/Fuzz/CheckFuzzScript.sh` (the
  `FuzzScript` ctest) for the new option.
- Verify: tidy shard times, fuzz job time, and that a deliberately introduced tidy finding fails the right shard
  (revert before merging).

### Session 3 — tests in parallel

Goal: test steps shrink to their longest single test.

- Mark tests that must not share the machine: every `benchmark`-labelled test (`Benchmark_*`, frame-time gates)
  gets `RUN_SERIAL TRUE`; review the editor integration tests (TCP ports, Xvfb display) and GPU tests that write to
  shared output directories, adding `RESOURCE_LOCK` where two tests touch the same files.
- CI: `ctest -j "$(nproc)"` (bash `nproc`/`sysctl -n hw.ncpu`; on Windows `$env:NUMBER_OF_PROCESSORS`) or set
  `CTEST_PARALLEL_LEVEL` in the job env. `scripts/test.sh` can do the same locally.
- lavapipe: start with `-j2` and `LP_NUM_THREADS=2` per test (llvmpipe otherwise uses every core per process);
  watch three runs for flakes before raising.
- `ReleasePackaging`: confirm the cause first (time `package_release.py` with `--with-engine` on Linux Debug). Then
  either register the test only in Release-style builds, or let tests pass a fast compression level (e.g. an
  environment variable read by `archive()`; `tarfile` gz level 1) while release builds keep the default.
- Shard `BasaltTests` into 3–4 ctest entries with doctest source-file filters (`-sf=` / `-sfe=`), split by measured
  time (2026-10-25 local: 40 s over 51 files; CoreTests 6.6 s, PhysicsTests 6.4 s, EditorHistoryTests 4.0 s, the
  rest ≤ 2.6 s). Keep a catch-all entry that excludes the named files so new test files always run. Check the
  coverage script and sanitizer environment still apply to every shard (`BASALT_SANITIZER_ENVIRONMENT`).
- Verify: three green runs on every platform; record Test-step times below.

### Session 4 — fewer jobs, earlier feedback

- Remove ubuntu/Debug: ASan (Clang Debug) runs the same headless tests, and the GCC Release build keeps GCC's
  warnings (memory: CI GCC flags -Wshadow / -Wmaybe-uninitialized). Move `BASALT_PROFILE=ON` (Tracy) to the ASan job.
  Residual risk: a GCC-only warning in Debug-only code — check with g++ locally per the GCC CI memory.
- Coverage: run on `push` to `main` only (`if: github.event_name == 'push'`); it is informational, never a gate.
- Early format job (~1 min, ubuntu, `clang-format` from apt at the version `cmake/CheckFormat.cmake` expects) so
  style failures appear before the macOS build; keep or drop the `CodeFormat` ctest on macOS Debug accordingly.
- Optional, discuss first: fold the per-push fuzz run into the ASan job (`BASALT_FUZZ=ON` adds
  `-fsanitize=fuzzer-no-link` everywhere, so the ASan tests run instrumented binaries; saves one full Clang ASan
  build per run).
- Keep both Windows configurations (memory: a Release-only double free on Windows) and both macOS ones (cheap, not
  on the critical path).

### Session 5 — maintenance

- Composite action `.github/actions/setup-basalt` (inputs: extra apt packages, Vulkan SDK on/off) holding the base
  Ubuntu package list, the macOS brew line and the Windows trio; use it from all three workflows. Alternatively put
  the base package list in `scripts/ci-apt-install.sh` and pass only extras.
- `fuzz-nightly.yml`: one build job uploads `build-fuzz/bin/Fuzz*` as an artifact; the matrix jobs download and run
  them (no compiler needed there). Optional once ccache makes the rebuild cheap.
- `paths-ignore` for files no test reads (`ROADMAP.md`, `README.md`, `.claude/**`); not `Docs/**`, which the
  `GeneratedDocs` and `LuaTypeDefinitions` tests check.

### Nightly fuzz findings (separate PRs)

From nightly run 40077058550 (2026-10-24):

- **FuzzGltf**: UBSan misaligned load in `cgltf.h:1566` (`unsigned short` from an odd address). Third-party: decide
  between a cgltf fix upstream/pin bump, a narrow `ubsan.supp` entry (currently empty), or copying unaligned buffers
  before cgltf reads them. Crash input is in the run's `fuzz-crashes-FuzzGltf` artifact; add it as a regression test.
- **FuzzCommand** (#58): died with SIGTRAP outside libFuzzer's reporting. A run with `handle_sigtrap=1` and a Jolt assert
  hook (40223333979) showed Jolt's `JobSystemWithBarrier` assert `job_ptr != nullptr && job_ptr->IsDone()` after
  `play.step`: on Linux its barrier waits in `sem_wait`, which libFuzzer's timer signal (handler installed without
  `SA_RESTART`) interrupts with EINTR, which Jolt ignores, so a physics step continued past running jobs. Input-independent; not a command bug. `PhysicsJobs` now has its
  own mutex/condition-variable barrier, and failed Jolt asserts are logged like Basalt's.

## Progress log

Record per session: PR number, measured job times after merge, anything that deviated from the plan.

- 2026-10-25: plan written from the baseline above.
- 2026-10-25, session 1 (#56), deviations from the plan:
  - No `cmake/CompilerCache.cmake`: CMake reads `CMAKE_<LANG>_COMPILER_LAUNCHER` from the environment, so the configures
    in `scripts/coverage.sh` and `scripts/fuzz.sh` get the launcher too. `cmake/Dependencies.cmake` adds what a launcher
    needs: MSVC `/Z7` for Debug and RelWithDebInfo when a launcher is set, Jolt's own `/Zi` off, and Jolt's precompiled
    header off (its 130 sources were uncacheable). No `CCACHE_BASEDIR` (the workspace path never changes).
    `CCACHE_COMPILERCHECK=content` is set in the workflows: the action sets it only on macOS and Windows.
  - Compiler caches are pruned of files unused for 7 days (`evict-old-files: 7d`), not per job: with `job`, a failed or
    cancelled job saved a smaller cache that the next run restored. Each is capped at about twice one build (first
    saves: Linux Debug 277 MB, macOS Debug 228 MB, macOS Release 58 MB).
  - Dependency sources: not `FETCHCONTENT_BASE_DIR` (whose stamps live in the `-subbuild` directories) but
    `FETCHCONTENT_SOURCE_DIR_<NAME>` for each `<name>-src` in `BASALT_DEPS_SOURCE_DIR` (`cmake/DependencySources.cmake`),
    and not `.deps` but `_deps` (so `coverage.sh`'s ignore pattern still applies). On a miss,
    `scripts/ci-fetch-deps.sh` downloads every dependency, Tracy and nativefiledialog included, into `_deps` before the
    build's configure, so hits and misses compile the same paths and every job that fetches saves the same set; the cache
    is saved right after the fetch. The directory is stamped with the hash of `Dependencies.cmake` and ignored when it
    differs; `scripts/ci-check-caches.sh` fails a job whose configure still downloaded something or lacks the launcher.
    Without `.git` the sources are about 414 MB on disk and about 180-230 MB as a cache entry per OS, instead of the
    1.2 GB entry each job restored before.
  - The first push showed Git Bash's `cp -R` cannot copy zstd's test symlinks on Windows (sources are now trimmed in
    place), and the second that `tr | grep -q` under `pipefail` failed the launcher check on macOS (SIGPIPE).
  - Measured on run 40200108108 (warm caches, 7c5c7ce): 22 min wall, down from 25-36. Compiler-cache hits 90-96% (ccache)
    and 98% (sccache, no uncacheable calls); dependency sources restore in 2-8 s. Build steps, baseline -> now: lavapipe
    788 s -> 11 s, Linux Debug 573 -> 70, ASan 391 -> 44, macOS Debug 392 -> 21, macOS Release 309 -> 9, Windows Debug
    671 -> 116, Windows Release 886 -> 95. Whole jobs: lavapipe (now also the Linux Release tests) 14 min, fuzz 16 min
    (15 of them running the targets), coverage 6 min. clang-tidy is now the critical path at 22 min (Build 1295 s,
    nearly all tidy, which is never cached): session 2.
  - The 20-30 misses per job are to be explained in session 2 (generated sources are the likely ones). Several caches
    filled their cap on this run (Linux Debug, ASan, coverage) because they still hold the objects of the earlier
    cold-path runs; ccache drops the oldest first.
- 2026-10-25, session 2, deviations from the plan:
  - The 20-30 compiler-cache misses per job left from session 1 are CMake's configure probes (`try_compile` checks and
    compiler detection, about 30 per configure), which compile in freshly named `CMakeScratch/TryCompile-*` directories
    and so never hit. On the last runs of #56 every build translation unit hit (e.g. clang-tidy: 526 calls = 496 build
    units + 30 probes, 28 misses; ubuntu/Debug: 527 = 497 + 30, 30 misses). Nothing to fix.
  - Shards are not split by module regex. `basalt_set_warnings` records each target it touches and a deferred call at
    the end of the configure writes them to `<build>/ClangTidyTargets.txt`; `scripts/clang_tidy.py` takes those
    targets' entries from `compile_commands.json` (by object path, `CMakeFiles/<target>.dir/`) and deals the sorted
    files round-robin over the shards. That is exactly the set `BASALT_CLANG_TIDY=ON` checks, and every file is in
    exactly one shard by construction, so the checker step became the `ClangTidyScript` ctest (selection against every
    compiled Basalt source, shard coverage, argument checks, and a stand-in clang-tidy that logs every file it checks,
    for findings and crashes); the tidy jobs run it too, as theirs is the only tree with fuzz and GPU targets. The script
    also fails a listed target without compile commands, a finding that exits 0 (a lost `WarningsAsErrors`) and a
    `.clang-tidy` that `--verify-config` rejects (a misspelled check is otherwise ignored), except for unknown checks it
    disables: those exist only in newer releases than CI's clang-tidy 18. No `run-clang-tidy`: the
    script prints each failing file's output in one block. Shards come from `strategy.job-index`/`job-total`.
  - Test sources stay checked (the user's open question; CI has caught real findings in `Tests/Source`). The tidy
    configure also enables `BASALT_GPU_TESTS` and `BASALT_FUZZ`, so the GPU test and fuzz target sources are checked
    for the first time, and the header filter now covers `Tests/Rendering`, `Tests/Benchmarks` and `Tests/Fuzz`.
    Their findings (implicit widening, integer division, an intended empty `catch`) are fixed, and
    `Tests/Rendering/.clang-tidy` turns off `bugprone-unchecked-optional-access` as `Tests/Source/.clang-tidy` does
    (doctest's `REQUIRE` ends a test, which the check cannot see; clang-tidy 18 with libstdc++ flagged more of these
    than Homebrew's 23 locally).
  - Shard check (run 40212392455): planted `modernize-use-nullptr` findings in `Tests/Source/SceneTests.cpp` and
    `Core/Log.cpp` were reported by exactly shards 1 and 3 (shard 2 failed on `ViewportGPU.cpp`'s optional-access
    findings, now turned off by `Tests/Rendering/.clang-tidy`); the commit was dropped.
  - The tidy jobs compile nothing but the shaders, so they have no compiler cache (launcher variables cleared for the job).
  - Fuzzing: `FUZZ_JOBS` (CI: `nproc`, 4) runs targets through `xargs -P`, since `wait -n` needs bash 4.3 and macOS
    ships 3.2. Concurrent output interleaves, so each target prints one status line as it finishes and the summaries
    follow in target order. Summaries now include `stat::peak_rss_mb` (250-650 MB on CI) and the `DONE` line, which the
    old pattern never matched (libFuzzer writes a tab there); a clean exit without it, a kill by a signal (SIGKILL is
    flagged as the likely out-of-memory killer) and a target that never finished fail the run. Concurrent targets get
    `-rss_limit_mb=3072` instead of 4096 (16 GB runners); one at a time (the nightly) keeps 4096. Pushes keep 60 s per
    target.
  - Run 40215677307 hit the nightly FuzzCommand finding on a push (killed by SIGTRAP after about 10 s, no input saved):
    libFuzzer does not handle SIGTRAP, which a breakpoint-style assert raises (`BS_DEBUGBREAK` is `__builtin_trap`, a
    SIGILL on x86, so likely a dependency's). `fuzz.sh` now sets ASan's `handle_sigtrap=1`, so such a trap is reported
    with a stack trace and its input saved; the fix itself stays in the planned FuzzCommand PR (it was Jolt's barrier, #58).
  - Measured on run 40217059412 (e10c9cc, 239 files tidied): 13 min wall, down from 22. clang-tidy shards 355/472/477 s
    (tidy itself 320-449 s; round-robin balances file counts, not cost), fuzz 366 s (fuzzing 336 s for 13 targets, 4 at
    a time). lavapipe (772 s, of which GPU tests 656 s run serially) is now the critical path: session 3.
