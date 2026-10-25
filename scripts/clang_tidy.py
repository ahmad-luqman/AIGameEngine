#!/usr/bin/env python3
"""Runs clang-tidy (checks in .clang-tidy) over Basalt's own translation units, or one shard of them.

The files are the sources of every target cmake/CompilerWarnings.cmake gives Basalt's warnings, whose names the
configure writes to <build>/ClangTidyTargets.txt: the same set -DBASALT_CLANG_TIDY=ON checks while compiling, so
third-party code is never checked. CI splits them over several jobs with --shard; files are dealt out round-robin in
path order, so every file belongs to exactly one shard however the sources change. Only a configured build tree is
needed, plus its generated sources (cmake --build <build> --target BasaltShaders for EmbeddedShaders.cpp).

Usage: scripts/clang_tidy.py [-p build] [--shard I/N] [-j jobs] [--clang-tidy exe] [--list] [file ...]
Files restrict the run to those translation units, e.g. the ones a change touched.
"""

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys

TARGETS_FILE = "ClangTidyTargets.txt"
# Tried in order when neither --clang-tidy nor CLANG_TIDY names one (cmake/CompilerWarnings.cmake searches the same).
CANDIDATES = ["clang-tidy", "clang-tidy-18", "clang-tidy-19", "/opt/homebrew/opt/llvm/bin/clang-tidy"]


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
    # CMake writes each object's path (CMakeFiles/<target>.dir/...) as "output" since 3.20 with Ninja and Makefiles; the
    # file name alone cannot tell an engine source from a third-party one built inside the source tree.
    if any("output" not in entry for entry in commands):
        sys.exit(f"{commands_path} has entries without an 'output' (needs CMake >= 3.20 with Ninja or Makefiles)")
    owned = re.compile(r"(^|/)CMakeFiles/(" + "|".join(map(re.escape, targets)) + r")\.dir/")
    # A source compiled into two targets (some editor sources are also in BasaltTests) is listed once: clang-tidy checks
    # every compile command it finds for a file.
    sources = {
        os.path.realpath(os.path.join(entry["directory"], entry["file"]))
        for entry in commands
        if owned.search(entry["output"].replace("\\", "/"))
    }
    if not sources:
        sys.exit(f"No compile command in {commands_path} belongs to {', '.join(targets)}")
    return sorted(sources)


def find_clang_tidy(requested):
    for candidate in [requested, os.environ.get("CLANG_TIDY")] + CANDIDATES:
        if candidate and shutil.which(candidate):
            return shutil.which(candidate)
    sys.exit("clang-tidy not found: install it, or pass --clang-tidy or set CLANG_TIDY")


def run_clang_tidy(clang_tidy, build_dir, source):
    result = subprocess.run([clang_tidy, "-p", build_dir, "--quiet", source], stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, errors="replace", check=False)
    return result.returncode, result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("-p", dest="build_dir", default="build", help="configured build tree (default: build)")
    parser.add_argument("--shard", type=parse_shard, default=(1, 1), help="check only shard I of N")
    parser.add_argument("-j", dest="jobs", type=parse_jobs, default=os.cpu_count() or 1, help="parallel clang-tidy runs")
    parser.add_argument("--clang-tidy", dest="clang_tidy", help="clang-tidy executable (default: CLANG_TIDY or PATH)")
    parser.add_argument("--list", action="store_true", help="print the selected files instead of checking them")
    parser.add_argument("files", nargs="*", help="check only these translation units")
    args = parser.parse_args()

    sources = select_sources(args.build_dir)
    if args.files:
        requested = {os.path.realpath(path) for path in args.files}
        unknown = sorted(requested - set(sources))
        if unknown:
            sys.exit("Not translation units of Basalt's targets in " + args.build_dir + ":\n  " + "\n  ".join(unknown))
        sources = [source for source in sources if source in requested]
    index, count = args.shard
    selected = sources[index - 1::count]
    if not selected:
        sys.exit(f"Shard {index}/{count} has no files (only {len(sources)} selected)")
    if args.list:
        print("\n".join(selected))
        return 0

    clang_tidy = find_clang_tidy(args.clang_tidy)
    version = subprocess.run([clang_tidy, "--version"], stdout=subprocess.PIPE, text=True, check=False).stdout
    version_line = next((line.strip() for line in version.splitlines() if "version" in line), clang_tidy)
    print(f"{version_line}: {len(selected)} of {len(sources)} files (shard {index}/{count}), {args.jobs} at a time",
        flush=True)
    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {executor.submit(run_clang_tidy, clang_tidy, args.build_dir, source): source for source in selected}
        for future in concurrent.futures.as_completed(futures):
            source = futures[future]
            code, output = future.result()
            # .clang-tidy makes every finding an error, so a clean file exits 0; its output (counts of warnings in
            # third-party headers, suppressed by the header filter) is noise.
            if code != 0:
                failed.append(source)
                print(f"=== {os.path.relpath(source)}: clang-tidy exited with {code}\n{output.rstrip()}", flush=True)
    if failed:
        print(f"clang-tidy failed on {len(failed)} of {len(selected)} files:", file=sys.stderr)
        for source in sorted(failed):
            print(f"  {os.path.relpath(source)}", file=sys.stderr)
        return 1
    print(f"clang-tidy passed on {len(selected)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
