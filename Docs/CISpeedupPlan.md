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
| 3 | Parallel ctest, fast `ReleasePackaging`, shard `BasaltTests` | Yes (test steps) | Merged (#59) |
| 4 | Remove redundant jobs; early format job; coverage on `main` only | Runner minutes, faster feedback | In review (#62) |
| 5 | Shared setup action; nightly fuzz builds once | Maintenance | Todo |
| — | Nightly fuzz findings (FuzzGltf, FuzzCommand) | Separate bug-fix PRs | FuzzCommand merged (#58); FuzzGltf merged (#61) |

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
  style failures appear before the macOS build; keep or drop the `CodeFormat` ctest on macOS Debug accordingly. (Done
  differently: a clang-format pinned from PyPI, see the progress log.)
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
  Fixed in #61 (see the FuzzGltf bullet below): the GLB's JSON chunk was not padded.
- **FuzzCommand** (#58): died with SIGTRAP outside libFuzzer's reporting. A run with `handle_sigtrap=1` and a Jolt assert
  hook (40223333979) showed Jolt's `JobSystemWithBarrier` assert `job_ptr != nullptr && job_ptr->IsDone()` after
  `play.step`: on Linux its barrier waits in `sem_wait`, which libFuzzer's timer signal (handler installed without
  `SA_RESTART`) interrupts with EINTR, which Jolt ignores, so a physics step continued past running jobs. Input-independent; not a command bug. `PhysicsJobs` now has its
  own mutex/condition-variable barrier, and failed Jolt asserts are logged like Basalt's.
- **FuzzGltf, fixed together in #61** (the misaligned load above, and timeouts in nightly runs 40854373678 on
  2026-10-29 and 41189784641 on 2026-10-31). Each input is now a fuzz seed in `Tests/Fuzz/Corpus/FuzzGltf`, and each has
  a doctest rebuilding it. Times are from the fuzz build (Debug with ASan, UBSan and coverage):
  - The misaligned load came from a GLB whose JSON chunk was 1241 bytes. GLB pads every chunk to 4 bytes, but cgltf
    neither checks that nor copies the binary chunk, so accessor offsets aligned within the buffer were odd addresses in
    memory. The importer rejects such GLBs (in `CheckAccessorAlignment`) before `cgltf_validate`.
  - 46116 zero-filled indices over 3 vertices: `meshopt_optimizeVertexCache` rescans the remaining triangles of every
    cached vertex after each triangle it emits, so its time grows with the squared vertex valences (22 s for one vertex
    in every triangle); the simplifier's edge scans grow the same way. `MeshOptimizer::Optimize` now keeps the triangle
    order (still ordering vertices for fetch) and the importer skips LODs, with a warning, when the squared valences sum
    to more than 16 per index plus an allowance of 2^22 shared by the model (review found eight primitives sharing one
    busy index accessor taking 12 s with a per-primitive allowance, and 64 per index too generous once the simplifier's
    passes count).
  - 4,000,008 zero-filled positions without indices: linear, but 27 s per run (simplifier 44%, tangents 20%,
    optimization 18%; libFuzzer runs an input twice when its allocations outnumber its frees, to look for leaks). The
    zero-filled limit drops from 2^22 to 2^16 elements, which import in 0.45 s per run.
  - Found by review: morph targets multiplied that work (each target with POSITION but no NORMAL deltas generates the
    normals again; 64 targets over 65535 zero-filled positions took 10 s), so zero-filled elements count once more per
    target; and a byteStride smaller than the element (forbidden by the specification) let a few bytes declare many
    vertices, which is now rejected.
  - Known, bounded and not fixed: primitives may share accessors (multi-material exports do), and each primitive
    processes everything it uses, at about 7 us per index in the fuzz build. GLBs of 14-15 KB whose 100-250 primitives
    share one 16x16 grid's indices take 2.7-5 s per run there (Release is many times faster); with the leak check's
    second run and a slower runner that reaches the 10 s timeout. `scripts/fuzz.sh` sets no `-max_len`, so the largest seed
    (`MixamoRig.gltf`, 12.3 KB) sets it. Follow-ups if it fires: give primitives that use the same accessors the earlier
    submesh's ranges with their own material, or compact each primitive to the vertices its indices use before
    generating normals and tangents.

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
- 2026-10-26, session 3 (#59), deviations from the plan. Measured first on a throwaway branch (run 40301624736); serial
  baselines from `main` (run 40233518562):
  - `ReleasePackaging`: confirmed. 119.5 of `package_release.py`'s 122 s on Linux Debug went to zlib, gzipping the
    240-300 MB tools (DWARF) at level 9; level 6 took a third of the time for 1% more bytes, level 1 a tenth for 13% more
    (`BasaltEditor`: 34.8 s / 87 MB, 10.7 s / 88 MB, 3.4 s / 98 MB). `package_release.py --gzip-level` (default 9, so
    releases are unchanged; other hosts warn that it does nothing) writes the Linux `.tar.gz` with `tarfile` directly
    (`shutil.make_archive` always uses 9). On Linux the test passes 1 and checks the gzip header; on every host but
    Windows it also runs the tarball code (header level, names, executable bits, the AppRun link). macOS (`ditto`) and Windows (zip; MSVC keeps debug
    information in separate `.pdb` files, which are not packaged) were never slow. `Saved/` (sample replays) no longer
    reaches the tools archive.
  - `BasaltTests` is five ctest entries: `BasaltTests_Editor`, `_Physics`, `_Animation` and `_Core` name their test files
    in `Tests/CMakeLists.txt`, and `BasaltTests` runs every other file through the same patterns as
    `--source-file-exclude`, so a new file always runs. Split by the per-file times of Windows Debug and ASan (doctest's
    XML reporter with `--duration`, used for proportions: in one process, 137 and 159 s), planned at 20-37 s per entry;
    in the first PR run, side by side, they took 34-46 s (ASan) and 35-61 s (Windows Debug). Configure fails on a
    shard with no files, a file listed twice, or a name that does not match exactly one file (doctest
    matches the pattern against the whole path, ignoring case: `*MathTests.cpp` also takes `RenderMathTests.cpp`).
    BasaltTests fails a run whose filters select no test case (doctest would report success), and the `BasaltTestShards`
    test lists each registered entry's test cases, fails unless every case runs exactly once, and checks that the
    entries are every ctest entry running the binary, differ only in their file filter, and share labels, timeout,
    environment (the sanitizer options) and working directory. All 51 test files also
    pass alone, each in its own process (macOS Debug). The per-process scratch directory now includes the process ID,
    and two `CoreTests` cases that used a fixed temporary path use it too.
  - No `RESOURCE_LOCK`. Tests use the source projects read-only (the runtime never writes a project; the CLI auto-saves
    only with `--scene`, which only McpIntegration and PrefabFeature pass, on their own copies, and the source-project
    batches never save) or fresh temporary copies. The two exceptions, the replays FeatureTest and SampleTetrisReplay
    record into their projects' `Saved/`, are left out of the copies other tests make (ArchiveExport,
    AssetPipelineFeature, PrefabFeature, and `package_release.py`'s copy of `Samples/`). `EditorViewport` reserves a free port and `xvfb-run -a` takes a free display. The benchmarks (and the
    local-only windowed render) are `RUN_SERIAL`.
  - lavapipe: running GPU tests side by side barely helps, because lavapipe already uses every core. Without the
    benchmark they took 634 s one at a time, and side by side 559 s (`-j2`, `LP_NUM_THREADS=2`), 493 s (`-j4`, 1), 486 s
    (`-j3`, 2) and 497 s (`-j4`, lavapipe's default of a thread per core); at `-j4`, LocalShadowRender, which starts the
    runtime again and again, went from 102 to about 320 s. So the job is split over two runners (`gpu-lavapipe` matrix
    `part`, selected with one regular expression as `-R`/`-E`, so new GPU tests land in part 2; the registration check
    fails on a stale or unlabelled name), each `-j2` with `LP_NUM_THREADS=2`. The first run's GPU steps took 175 s (part
    1: the Python-driven renders, the benchmark) and 313 s (part 2). Moving the three longest GPU test executables into
    part 1 overshot (347 / 220 s), also because ctest, with no timing history, started LocalShadowRender last. The long
    Python-driven tests and TemporalAAGPU now have a `COST`, so they start first, and only TemporalAAGPU moved: 172 /
    284 s (run 40311999141). Part 2 could shed another test or two, but its job already ends with the clang-tidy shards
    (which have no compiler cache, 9-10 min), so that would not shorten the run.
  - Headless tests run one per core everywhere (`scripts/test.sh` too, overridable with `-j` or `CTEST_PARALLEL_LEVEL`;
    `scripts/coverage.sh`, whose profiles are already per process), with `--no-tests=error`. Three `-j4` runs each on
    Linux Debug, ASan and Windows Debug passed before the shards; under `-j4` the single `BasaltTests` grew from 95 to
    215 s (ASan) and from 139 to 166-183 s (Windows Debug).
  - First PR run (40306777393, bf1b712; builds partly cold): 10.5 min wall, down from 13 (session 2's run 40217059412;
    `main`'s 40233518562 took 31.6 min with a cold lavapipe build). Test steps, before -> after:
    ASan 321 -> 102 s, Linux Debug 271 -> 65, Windows Debug 246 -> 111, macOS Debug 114 -> 75, macOS Release 54 -> 24,
    Windows Release 54 -> 27, lavapipe headless 62 -> 10 and GPU 657 -> 175 / 313. The next two runs took 11.6 and 10.2
    min, their builds also partly cold (each changed widely included test or engine headers); the ASan job of the last
    ran every test about twice as slowly as the run before (runner variance). The merge of #59 ran no CI on `main` (its
    head commit said `[skip ci]`), so the next times come from #61's first push (run 41238549772, caches restored from
    `main`, but it changed `MeshSource.h`, which many sources include): 12.8 min wall, set by windows/Release (12.8), ASan (11.7) and lavapipe part 1
    (11.6); lavapipe part 2 9.9, fuzz 10.3, macOS Release 10.4 and Debug 9.9, windows/Debug 9.2, ubuntu/Debug 8.3,
    coverage 6.8, clang-tidy shards 6.0, 7.2 and 5.5.
- 2026-11-01, session 4 (#62), deviations from the plan:
  - ubuntu/Debug is gone, and with it every Linux entry of the `build-and-test` matrix (ubuntu/Release already ran in the
    lavapipe job), so that job lost its Linux install step and Tracy switch. Tracy (`BASALT_PROFILE=ON`) moved to the ASan
    job, which on pull requests is now CI's only Linux Debug build of the engine, apps and tests (fuzz builds only its
    targets). A new step there fails when Tracy is not enabled (`BASALT_PROFILE` quietly does nothing without Tracy's
    target), when Tracy's crash handlers are on, or when a test a missing tool or rename would drop (`BasaltTests`,
    `BasaltTestShards`, `FeatureTest`, `PhysicsDeterminism`) is not registered. The first CI run's ASan job passed with
    LeakSanitizer on (local macOS runs have only ASan and UBSan; Apple's runtime has no LSan).
  - Lost with the job: `CrashReport` in a Linux Debug build (it skips under ASan), and GCC at -O0 (GCC now compiles only
    Release, in lavapipe, and Dist, in `release.yml`), so a GCC warning that shows only in unoptimized builds shows up only
    locally; no sources use `BS_DEBUG`, and asserts stay on in Release. Lavapipe part 1 is now the only Linux run of
    `CrashReport`, so its registration check lists it and its headless step fails when ctest reports it skipped (ctest
    counts a skip as a pass). While a profiler is connected, Tracy installs crash handlers (signals on Linux, an
    exception filter on Windows); sanitizer builds turn them off, so CI compiles Tracy only without them.
  - Coverage runs on pushes to `main` and on manual runs (`workflow_dispatch`, new, which runs every job on a chosen
    branch; manual runs have their own concurrency group, so they and pushes never cancel each other). Its numbers were
    never a gate, but a test that fails only in its instrumented build now first fails on `main`. Coverage runs nowhere
    else, so a merge whose head commit says `[skip ci]`, or whose run a quickly following push to `main` cancels
    (`cancel-in-progress`), leaves that commit without a report.
  - The format job does not take clang-format from apt: Ubuntu 24.04's clang-format 18 reports 9 places in the current
    tree that 22.1.8 and 23.1.3 (PyPI) and 23.1.4 (Homebrew) all accept. The job installs the `CLANG_FORMAT_VERSION`
    wheel (23.1.3, PyPI's newest) into a virtual environment (Ubuntu's system pip refuses installs), checks the reported
    version, and runs `cmake/CheckFormat.cmake` and `Tests/Format/CheckFormatBatches.cmake` directly (no configure, so no
    dependency sources). They replace the `CodeFormat` and `CheckFormatBatches` ctests on macOS Debug, which no longer
    installs clang-format. No job waits for it. `CheckFormat.cmake` now reports how many files it checked and with which
    version.
  - Locally, `scripts/format.sh` (which takes `CLANG_FORMAT`) and the configure that registers `CodeFormat` warn when
    their clang-format's major version differs from the pin, through one script, `cmake/CheckClangFormatVersion.cmake`,
    which reads the pin from `ci.yml` (quoted, commented or CRLF) and only warns when the workflow is missing. The format
    job runs it with `STRICT=ON`, which fails instead, so a pin line it can no longer read is caught; runs its test
    (`CheckFormatVersion`, also a ctest); and runs `scripts/format.sh` itself, which must change nothing and warn about
    nothing.
  - The first run's format job failed in `CheckFormatBatches`, a bug the macOS ctest had hidden: CMake wraps `message()`
    text, and on the runner's 63-character path "failed to run" broke over two lines (44-51 and 59-64 characters split
    it; from about 65 the path takes a line of its own, as on macOS's build path). The script now collapses whitespace
    before matching, and the launch-failure case runs with paths of 33 to 73 characters (it fails without the fix).
    Review also found that its misformatted-file case never reached the last batch (`File101.cpp` sorted fourth, after
    `File1`, `File10` and `File100`); the generated names are now zero-padded, and misformatted first, 100th and last
    files are checked too (dropping a file at a batch boundary, or keeping only the last batch's verdict, passed before).
  - Not done: folding the per-push fuzz run into the ASan job (optional in the plan, left for a later discussion).
  - First run (41320853453, 331ffc6, failed in the format job): 9.2 min wall, set by clang-tidy shard 2 (9.1 min; the
    shards took 4.6-9.1 min) with fuzz 5.7, ASan 5.9 and lavapipe 5.0 / 3.4; Windows 4.3 / 2.9 and macOS 2.2 / 1.3
    (Debug / Release); format 11 s. Second run (41323447289, dfa7a49, green; the commit changed
    `cmake/Dependencies.cmake`, which keys the dependency-source cache, so every job fetched the sources, 88-117 s): clang-tidy 7.5-10.5 min, fuzz 7.7, lavapipe
    5.3 / 7.0, Windows 5.7 / 5.1, macOS 4.0 / 3.1, ASan 4.7; format 9 s.
