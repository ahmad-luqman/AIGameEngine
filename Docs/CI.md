# Continuous integration

What runs where in `.github/`, how its caches work, and what it does not check. The workflow files explain each step;
this page is the overview and the list of gaps to keep in mind when a change depends on one of them.

## Workflows

| Workflow | Runs on | Jobs |
|----------|---------|------|
| `ci.yml` | Every pull request and push to `main`; manual runs (`gh workflow run ci.yml --ref <branch>`) | `format`; Windows and macOS × Debug/Release; Linux ASan+UBSan (Clang Debug, with Tracy); clang-tidy in three shards; Linux Release with GCC in two lavapipe parts (part 1: the headless tests, the benchmarks and the GPU tests named in `GPU_PART_1`; part 2: every other GPU test); fuzz (60 s per target); coverage (pushes to `main` and manual runs only) |
| `fuzz-nightly.yml` | 03:17 UTC every night; manual runs (`-f seconds=N`) | One job builds every fuzz target, then one job per target fuzzes it (1200 s by default), keeping each target's grown corpus in a cache. A failing target uploads `fuzz-crashes-<Target>` (crash inputs and the fuzzer log) |
| `release.yml` | `v*` tags (publishes a GitHub release); pull requests that change its inputs (listed in the file) and manual runs build and package without publishing | One packaging job per OS (Dist build, `scripts/package_release.py`), then `publish` for tags |

Every job that builds Basalt sets up its runner through `.github/actions/setup-basalt` (the Linux package list, the
Homebrew packages, the Windows Vulkan SDK, MSVC and Ninja, and the dependency sources). Pull requests that change the
action also run `release.yml`; `fuzz-nightly.yml` has no pull request trigger, so start it by hand after changing it (its
header says when and how).

A `ci.yml` run took about 10 minutes in November 2026, set by the slowest clang-tidy shard. `main` has no branch
protection, so renaming or removing jobs blocks nothing; check `gh api repos/ahmad-luqman/AIGameEngine/rulesets` and
`gh api repos/ahmad-luqman/AIGameEngine/branches/main/protection` first in case that changed.

## Caches

- **Compiler caches.** Every `ci.yml` job that compiles goes through ccache (sccache on Windows, where Debug objects then
  embed their debug information, `/Z7`), one cache per kind of job: the two lavapipe parts share one (part 1 saves it),
  and the nightly fuzz build restores, never saves, CI's fuzz cache. ccache drops files unused for a week. The clang-tidy
  jobs compile only shaders, and `release.yml`'s Dist builds run without a compiler cache.
- **Dependency sources.** One entry per OS, shared by every job and workflow, holds the pinned third-party sources
  (`scripts/ci-fetch-deps.sh`, used through `BASALT_DEPS_SOURCE_DIR` by `cmake/DependencySources.cmake`). Its key hashes
  `cmake/Dependencies.cmake` and `scripts/ci-fetch-deps.sh`, so a change to either makes every job of the first run of a
  pull request fetch the sources again (about 1.5-2 minutes each). That run saves an entry that only the pull request's
  later runs can read (manual runs on its branch save their own); other refs miss until `main` saves one.
- `scripts/ci-check-caches.sh` fails a job whose build downloaded a dependency or does not compile through its compiler
  cache, which would otherwise only show as a slower run.
- The repository's Actions caches share 10 GB; GitHub evicts the least recently used entries beyond that.

## What CI does not check

- **GCC without optimization.** GCC compiles only Release (lavapipe) and Dist (`release.yml`); Linux Debug is Clang under
  ASan. A GCC warning that appears only at `-O0` shows up only locally.
- **`CrashReport` in a Linux Debug build.** It skips under ASan, so its only Linux run is lavapipe part 1 (Release),
  which fails if the test is skipped.
- **Coverage on pull requests.** Coverage's instrumented build runs only on `main` and manual runs, so a failure that
  only it shows first appears on `main`. A merge whose head commit says `[skip ci]`, or whose run a quick following push
  to `main` cancels, leaves that commit without a report.
- **Real GPUs.** GPU tests run on Mesa's lavapipe only; the Windows and macOS jobs run none. Real hardware is checked by
  hand before releases (`Docs/TestedHardware.md`).
- **Long fuzzing on pull requests.** Each push fuzzes every target for 60 s; only the nightly runs long enough to reach
  deeper inputs. Import work grows with what an input declares: FuzzGltf inputs of about 15 KB whose 70 primitives each
  read a separate accessor over one 16x16 grid's indices take about 0.7 s per execution in the fuzz build, well within
  the 10 s limit (primitives that repeat the same accessors import once; see `Docs/AssetPipeline.md`).
- **Release packaging and the nightly between triggers.** `release.yml` runs only on tags, on pull requests touching its
  inputs, and by hand; `fuzz-nightly.yml` only nightly and by hand.

## Pitfalls

- Never end a pull request with a `[skip ci]` commit: GitHub skips the push of a rebase merge whose head commit says it,
  so `main` gets no run at all.
- The format job pins clang-format (`CLANG_FORMAT_VERSION` in `ci.yml`); other major versions format some code
  differently (see AGENTS.md).
- Ubuntu runners have CMake 3.31, while Windows runners and Homebrew have 4.x: start every `cmake -P` script with
  `cmake_minimum_required(VERSION 3.25)`.
