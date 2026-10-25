#!/usr/bin/env python3
"""Checks scripts/clang_tidy.py (ctest ClangTidyScript): which files it selects from this build tree, that its shards
cover them exactly once, its argument checks, and that a finding fails the run (with a stand-in clang-tidy).

Usage: CheckClangTidyScript.py <source dir> <build dir> <work dir>
"""
import json, os, shutil, stat, subprocess, sys

source_dir, build_dir, work_dir = sys.argv[1:4]
script = os.path.join(source_dir, "scripts", "clang_tidy.py")


def fail(message):
    raise SystemExit(f"FAIL: {message}")


def run(*arguments):
    return subprocess.run([sys.executable, "-B", script, *arguments], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, check=False)


def listed(*arguments):
    result = run("--list", *arguments)
    if result.returncode != 0:
        fail(f"--list {' '.join(arguments)} exited with {result.returncode}:\n{result.stdout}")
    return result.stdout.split()


# This build: every target with Basalt's warnings, and nothing else.
with open(os.path.join(build_dir, "ClangTidyTargets.txt"), encoding="utf-8") as file:
    targets = file.read().split()
for target in ("Basalt", "BasaltTests", "BasaltCLI"):
    if target not in targets:
        fail(f"ClangTidyTargets.txt lacks {target}: {targets}")
everything = listed("-p", build_dir)
if len(everything) != len(set(everything)) or everything != sorted(everything):
    fail("the selection is not a sorted list of unique files")
for path in everything:
    normalized = path.replace("\\", "/")
    if "/_deps/" in normalized:
        fail(f"a third-party source was selected: {path}")
names = {path.replace("\\", "/") for path in everything}
for expected in ("/Basalt/Source/Basalt/Scene/Scene.cpp", "/Tests/Source/SceneTests.cpp",
        "/Basalt/EmbeddedShaders/EmbeddedShaders.cpp"):
    if not any(name.endswith(expected) for name in names):
        fail(f"no selected file ends with {expected}")

# Shards: each file exactly once, sizes within one of each other.
for count in (1, 2, 3, 5):
    shards = [listed("-p", build_dir, "--shard", f"{index}/{count}") for index in range(1, count + 1)]
    combined = [path for shard in shards for path in shard]
    if sorted(combined) != everything:
        fail(f"{count} shards do not cover the selection exactly once")
    sizes = [len(shard) for shard in shards]
    if max(sizes) - min(sizes) > 1:
        fail(f"{count} shards are unbalanced: {sizes}")

for shard in ("0/3", "4/3", "1/0", "a/b", "3", "1/2/3"):
    result = run("--list", "-p", build_dir, "--shard", shard)
    if result.returncode == 0 or "expected I/N" not in result.stdout:
        fail(f"--shard {shard} was accepted or printed:\n{result.stdout}")
for jobs in ("0", "-1", "x"):
    result = run("--list", "-p", build_dir, "-j", jobs)
    if result.returncode == 0 or "positive integer" not in result.stdout:
        fail(f"-j {jobs} was accepted or printed:\n{result.stdout}")

# A stand-in build tree: one owned target, one third-party target, and a clang-tidy that fails on "Bad" files.
shutil.rmtree(work_dir, ignore_errors=True)
fake_build = os.path.join(work_dir, "build")
os.makedirs(fake_build)
sources = {name: os.path.join(work_dir, name) for name in ("Good.cpp", "Bad.cpp", "Other.cpp", "Third.cpp")}
for path in sources.values():
    open(path, "w").close()


def entry(name, target):
    return {"directory": fake_build, "command": f"c++ -c {sources[name]}", "file": sources[name],
        "output": f"Owned/CMakeFiles/{target}.dir/{name}.o"}


commands = [entry("Good.cpp", "Owned"), entry("Bad.cpp", "Owned"), entry("Other.cpp", "Owned"),
    entry("Good.cpp", "OwnedTests"), entry("Third.cpp", "Owned_deps")]
with open(os.path.join(fake_build, "compile_commands.json"), "w", encoding="utf-8") as file:
    json.dump(commands, file)
with open(os.path.join(fake_build, "ClangTidyTargets.txt"), "w", encoding="utf-8") as file:
    file.write("Owned\nOwnedTests\n")
fake_tidy = os.path.join(work_dir, "fake-clang-tidy")
with open(fake_tidy, "w", encoding="utf-8") as file:
    file.write(f"""#!{sys.executable}
import sys
if sys.argv[1:] == ["--version"]:
    print("fake clang-tidy version 1")
    sys.exit(0)
if sys.argv[1:3] != ["-p", {fake_build!r}]:
    print("unexpected arguments", sys.argv)
    sys.exit(3)
if "Bad" in sys.argv[-1]:
    print(sys.argv[-1] + ":1:1: error: planted finding [bugprone-planted]")
    sys.exit(1)
print("12 warnings generated.")
""")
os.chmod(fake_tidy, os.stat(fake_tidy).st_mode | stat.S_IXUSR)

selected = listed("-p", fake_build)
expected = sorted(os.path.realpath(sources[name]) for name in ("Good.cpp", "Bad.cpp", "Other.cpp"))
if selected != expected:
    fail(f"the stand-in tree selected {selected}, not {expected} (a target named like an owned one must not match)")

result = run("-p", fake_build, "-j", "2", "--clang-tidy", fake_tidy)
if result.returncode != 1 or "planted finding" not in result.stdout or "failed on 1 of 3 files" not in result.stdout:
    fail(f"a finding did not fail the run (exit {result.returncode}):\n{result.stdout}")
if "warnings generated" in result.stdout:
    fail(f"a clean file's output was printed:\n{result.stdout}")
result = run("-p", fake_build, "--clang-tidy", fake_tidy, sources["Good.cpp"], sources["Other.cpp"])
if result.returncode != 0 or "passed on 2 files" not in result.stdout:
    fail(f"a run restricted to clean files failed (exit {result.returncode}):\n{result.stdout}")
result = run("-p", fake_build, "--clang-tidy", fake_tidy, sources["Third.cpp"])
if result.returncode == 0 or "Not translation units" not in result.stdout:
    fail(f"a third-party file was accepted:\n{result.stdout}")

# A tree configured before ClangTidyTargets.txt existed, or a compile database without object paths.
os.remove(os.path.join(fake_build, "ClangTidyTargets.txt"))
result = run("--list", "-p", fake_build)
if result.returncode == 0 or "ClangTidyTargets.txt is missing" not in result.stdout:
    fail(f"a missing target list was accepted:\n{result.stdout}")
with open(os.path.join(fake_build, "ClangTidyTargets.txt"), "w", encoding="utf-8") as file:
    file.write("Owned\n")
for item in commands:
    del item["output"]
with open(os.path.join(fake_build, "compile_commands.json"), "w", encoding="utf-8") as file:
    json.dump(commands, file)
result = run("--list", "-p", fake_build)
if result.returncode == 0 or "without an 'output'" not in result.stdout:
    fail(f"entries without object paths were accepted:\n{result.stdout}")
print("clang_tidy.py checks passed")
