#!/usr/bin/env python3
"""Runs clang-tidy (checks in .clang-tidy) over Basalt's own translation units, or one shard of them.

The files are the sources of every target cmake/CompilerWarnings.cmake gives Basalt's warnings, whose names the
configure writes to <build>/ClangTidyTargets.txt: the same set -DBASALT_CLANG_TIDY=ON checks while compiling, so
third-party code is never checked. CI splits them over several jobs with --shard; files are dealt out round-robin in
path order, so every file belongs to exactly one shard however the sources change. Only a configured build tree is
needed, plus the shader blobs EmbeddedShaders.cpp includes (cmake --build <build> --target BasaltShaders).

Usage: scripts/clang_tidy.py [-p build] [--shard I/N] [-j jobs] [--clang-tidy exe] [--list] [file ...]
Files restrict the run to those translation units, e.g. the ones a change touched.
"""

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import signal
import subprocess
import sys

SOURCE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGETS_FILE = "ClangTidyTargets.txt"
# Tried in order when neither --clang-tidy nor CLANG_TIDY names one: cmake/CompilerWarnings.cmake's find_program names,
# Homebrew LLVM first as there (its HINTS come before PATH). Keep both lists in sync.
CANDIDATES = ["/opt/homebrew/opt/llvm/bin/clang-tidy", "clang-tidy", "clang-tidy-18", "clang-tidy-19"]
# A diagnostic from a check, e.g. "Foo.cpp:12:3: warning: ... [bugprone-foo]" or "[clang-diagnostic-c++20-extensions]".
FINDING = re.compile(r":\d+:\d+: (warning|error): .*\[[^\]\s]+\]$", re.MULTILINE)


def parse_shard(text):
    match = re.fullmatch(r"([0-9]+)/([0-9]+)", text)
    if not match or not 1 <= int(match.group(1)) <= int(match.group(2)):
        raise argparse.ArgumentTypeError(f"expected I/N with 1 <= I <= N, not '{text}'")
    return int(match.group(1)), int(match.group(2))


def parse_jobs(text):
    if not re.fullmatch(r"[1-9][0-9]*", text):
        raise argparse.ArgumentTypeError(f"expected a positive integer, not '{text}'")
    return int(text)


def select_sources(build_dir):
    """Returns the sorted, unique source paths of the targets in ClangTidyTargets.txt."""
    targets_path = os.path.join(build_dir, TARGETS_FILE)
    commands_path = os.path.join(build_dir, "compile_commands.json")
    for path in (targets_path, commands_path):
        if not os.path.isfile(path):
            sys.exit(f"{path} is missing: configure {build_dir} with this checkout's CMakeLists.txt first")
    with open(targets_path, encoding="utf-8") as file:
        targets = [line.strip() for line in file if line.strip()]
    with open(commands_path, encoding="utf-8") as file:
        commands = json.load(file)
    if not targets:
        sys.exit(f"{targets_path} names no targets")
    # The object path (CMakeFiles/<target>.dir/...) is the only link from an entry to its target: a source path cannot
    # tell an engine file from a third-party one, which CI keeps inside the checkout (_deps).
    if any("output" not in entry for entry in commands):
        sys.exit(f"{commands_path} has entries without an 'output': it was not written by this project's configure "
            "with Ninja or Makefiles")
    owned = re.compile(r"(^|/)CMakeFiles/(" + "|".join(map(re.escape, targets)) + r")\.dir/")
    sources = set()
    matched = set()
    for entry in commands:
        match = owned.search(entry["output"].replace("\\", "/"))
        if match:
            matched.add(match.group(2))
            # A source compiled into two targets (some editor sources are also in BasaltTests) is listed once:
            # clang-tidy checks every compile command it finds for a file.
            sources.add(os.path.realpath(os.path.join(entry["directory"], entry["file"])))
    # A target whose objects moved elsewhere (another generator or object layout) would otherwise drop out silently.
    missing = [target for target in targets if target not in matched]
    if missing:
        sys.exit(f"No compile command in {commands_path} belongs to {', '.join(missing)} (listed in {targets_path})")
    return sorted(sources)


def find_clang_tidy(requested):
    # An executable named on purpose (even an empty name) must exist: falling back would check with another version.
    source = "--clang-tidy" if requested is not None else "CLANG_TIDY"
    named = requested if requested is not None else os.environ.get("CLANG_TIDY")
    if named is not None:
        path = shutil.which(named) if named else None
        if not path:
            sys.exit(f"clang-tidy '{named}' (from {source}) was not found")
        return path
    for candidate in CANDIDATES:
        path = shutil.which(candidate)
        if path:
            return path
    sys.exit("clang-tidy not found: install it, or pass --clang-tidy or set CLANG_TIDY")


def run_clang_tidy(clang_tidy, build_dir, source):
    try:
        result = subprocess.run([clang_tidy, "-p", build_dir, "--quiet", source], stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, errors="replace", check=False)
    except OSError as error:
        return None, str(error)
    return result.returncode, result.stdout


def config_problems(output):
    """Returns the lines of --verify-config's output that are problems: all but unknown checks that .clang-tidy disables
    (they exist only in newer clang-tidy releases than CI's) and the all-clear."""
    with open(os.path.join(SOURCE_DIR, ".clang-tidy"), encoding="utf-8") as file:
        config = file.read()
    problems = []
    for line in filter(None, (line.strip() for line in output.splitlines())):
        unknown = re.search(r"unknown check '([^']+)'", line)
        if unknown and re.search(r"^\s*-" + re.escape(unknown.group(1)) + r"\s*,?\s*$", config, re.MULTILINE):
            continue
        if line != "No config errors detected.":
            problems.append(line)
    return problems


def failure_reason(code, output):
    """Returns why a file's run failed, or None when it is clean."""
    if code is None:
        return "clang-tidy did not start"
    if code < 0:
        return f"clang-tidy was killed by {signal.Signals(-code).name}" if -code in signal.valid_signals() \
            else f"clang-tidy was killed by signal {-code}"
    if code != 0:
        return f"clang-tidy exited with {code}"
    # .clang-tidy makes every finding an error. A finding that still exits 0 means a configuration lost that (a nested
    # .clang-tidy without InheritParentConfig, or the generated EmbeddedShaders.cpp of a build tree outside the checkout,
    # which no .clang-tidy covers). Other output of a clean file (warnings in third-party headers the filter hides) is noise.
    if FINDING.search(output):
        return "findings that are not errors (WarningsAsErrors?)"
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("-p", dest="build_dir", default="build", help="configured build tree (default: build)")
    parser.add_argument("--shard", type=parse_shard, default=(1, 1), help="check only shard I of N")
    parser.add_argument("-j", dest="jobs", type=parse_jobs, default=os.cpu_count() or 1, help="parallel clang-tidy runs")
    parser.add_argument("--clang-tidy", dest="clang_tidy",
        help="clang-tidy executable (default: CLANG_TIDY, else the first found of " + ", ".join(CANDIDATES) + ")")
    parser.add_argument("--list", action="store_true", help="print the selected files instead of checking them")
    parser.add_argument("files", nargs="*", help="check only these translation units")
    args = parser.parse_args()

    sources = select_sources(args.build_dir)
    if args.files:
        requested = {os.path.realpath(path) for path in args.files}
        unknown = sorted(requested - set(sources))
        if unknown:
            sys.exit(f"Not translation units of Basalt's targets in {args.build_dir}:\n  " + "\n  ".join(unknown))
        sources = [source for source in sources if source in requested]
    index, count = args.shard
    selected = sources[index - 1::count]
    if not selected:
        sys.exit(f"Shard {index}/{count} has no files (only {len(sources)} selected)")
    if args.list:
        print("\n".join(selected))
        return 0

    clang_tidy = find_clang_tidy(args.clang_tidy)
    version = subprocess.run([clang_tidy, "--version"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        check=False)
    if version.returncode != 0:
        sys.exit(f"{clang_tidy} --version exited with {version.returncode}:\n{version.stdout}")
    version_line = next((line.strip() for line in version.stdout.splitlines() if "version" in line), clang_tidy)
    # clang-tidy ignores a misspelled check or option in .clang-tidy, which would quietly stop checking it.
    config = subprocess.run([clang_tidy, "--verify-config"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        check=False, cwd=SOURCE_DIR)
    # An exit code of 1 with only tolerated lines is fine; anything else in the output is not.
    if config_problems(config.stdout) or (config.returncode != 0 and "unknown check" not in config.stdout):
        sys.exit(f"{clang_tidy} --verify-config rejected .clang-tidy:\n{config.stdout}")
    print(f"{version_line}: {len(selected)} of {len(sources)} files (shard {index}/{count}), {args.jobs} at a time",
        flush=True)
    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {executor.submit(run_clang_tidy, clang_tidy, args.build_dir, source): source for source in selected}
        for future in concurrent.futures.as_completed(futures):
            source = futures[future]
            code, output = future.result()
            reason = failure_reason(code, output)
            if reason:
                failed.append(source)
                print(f"=== {os.path.relpath(source)}: {reason}\n{output.rstrip()}", flush=True)
    if failed:
        print(f"clang-tidy failed on {len(failed)} of {len(selected)} files:", file=sys.stderr)
        for source in sorted(failed):
            print(f"  {os.path.relpath(source)}", file=sys.stderr)
        return 1
    print(f"clang-tidy passed on {len(selected)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
