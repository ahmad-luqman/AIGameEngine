#!/usr/bin/env python3
"""Checks scripts/clang_tidy.py (ctest ClangTidyScript): which files it selects from this build tree, that its shards
cover them exactly once, its argument checks, and how findings and failures end a run (with a stand-in clang-tidy).

Usage: CheckClangTidyScript.py <source dir> <build dir> <work dir>
"""
import json, os, shutil, stat, subprocess, sys

source_dir, build_dir, work_dir = sys.argv[1:4]
script = os.path.join(source_dir, "scripts", "clang_tidy.py")


def fail(message):
    raise SystemExit(f"FAIL: {message}")


def run(*arguments, env=None, cwd=None):
    return subprocess.run([sys.executable, "-B", script, *arguments], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, check=False, env=env, cwd=cwd)


def listed(*arguments, cwd=None):
    result = run("--list", *arguments, cwd=cwd)
    if result.returncode != 0:
        fail(f"--list {' '.join(arguments)} exited with {result.returncode}:\n{result.stdout}")
    return result.stdout.split()


def rejected(expected, *arguments, env=None):
    result = run(*arguments, env=env)
    if result.returncode == 0 or expected not in result.stdout:
        fail(f"'{' '.join(arguments)}' was accepted or did not print '{expected}':\n{result.stdout}")


# This build: every compile command of Basalt's own source directories, and nothing else.
with open(os.path.join(build_dir, "ClangTidyTargets.txt"), encoding="utf-8") as file:
    targets = file.read().split()
for target in ("Basalt", "BasaltTests", "BasaltCLI", "BasaltBenchmark"):
    if target not in targets:
        fail(f"ClangTidyTargets.txt lacks {target}: {targets}")
everything = listed("-p", build_dir)
if len(everything) != len(set(everything)) or everything != sorted(everything):
    fail("the selection is not a sorted list of unique files")
real_source = os.path.realpath(source_dir)
owned_dirs = [os.path.join(real_source, path) for path in ("Basalt/Source", "Basalt-Editor/Source", "Basalt-Runtime/Source",
    "Basalt-CLI/Source", "Tests/Source", "Tests/Rendering", "Tests/Benchmarks", "Tests/Fuzz")]
generated = os.path.join(os.path.realpath(build_dir), "Basalt", "EmbeddedShaders", "EmbeddedShaders.cpp")


def in_owned_dirs(path):
    return any(path.startswith(directory + os.sep) for directory in owned_dirs)


for path in everything:
    if path != generated and not in_owned_dirs(path):
        fail(f"a file outside Basalt's source directories was selected: {path}")
if generated not in everything:
    fail(f"{generated} was not selected")
# A coverage floor: a target dropping out of the list (or the filter) would leave its files unchecked.
with open(os.path.join(build_dir, "compile_commands.json"), encoding="utf-8") as file:
    compiled = {os.path.realpath(os.path.join(entry["directory"], entry["file"])) for entry in json.load(file)}
unchecked = sorted(path for path in compiled - set(everything) if in_owned_dirs(path))
if unchecked:
    fail("compiled Basalt sources are not selected:\n  " + "\n  ".join(unchecked))

# Shards: each file exactly once, sizes within one of each other.
for count in (1, 2, 3, 5):
    shards = [listed("-p", build_dir, "--shard", f"{index}/{count}") for index in range(1, count + 1)]
    if sorted(path for shard in shards for path in shard) != everything:
        fail(f"{count} shards do not cover the selection exactly once")
    sizes = [len(shard) for shard in shards]
    if max(sizes) - min(sizes) > 1:
        fail(f"{count} shards are unbalanced: {sizes}")

for shard in ("0/3", "4/3", "1/0", "a/b", "3", "1/2/3"):
    rejected("expected I/N", "--list", "-p", build_dir, "--shard", shard)
for jobs in ("0", "-1", "x"):
    rejected("positive integer", "--list", "-p", build_dir, "-j", jobs)

# A stand-in build tree: two owned targets sharing a source, a third-party target whose name extends an owned one, and a
# clang-tidy that logs each file and behaves by file name.
shutil.rmtree(work_dir, ignore_errors=True)
fake_build = os.path.join(work_dir, "build")
os.makedirs(fake_build)
owned_names = ("Good.cpp", "Bad.cpp", "Quiet.cpp", "Crash.cpp", "Other.cpp")
names = owned_names + ("Third.cpp",)
sources = {name: os.path.realpath(os.path.join(work_dir, name)) for name in names}
for path in sources.values():
    open(path, "w", encoding="utf-8").close()


def entry(name, target):
    return {"directory": fake_build, "command": f"c++ -c {sources[name]}", "file": sources[name],
        "output": f"Owned/CMakeFiles/{target}.dir/{name}.o"}


def write_tree(commands, targets):
    with open(os.path.join(fake_build, "compile_commands.json"), "w", encoding="utf-8") as file:
        json.dump(commands, file)
    with open(os.path.join(fake_build, "ClangTidyTargets.txt"), "w", encoding="utf-8") as file:
        file.write("".join(target + "\n" for target in targets))


commands = [entry(name, "Owned") for name in owned_names] + [entry("Good.cpp", "OwnedTests"), entry("Third.cpp", "Owned_deps")]
write_tree(commands, ["Owned", "OwnedTests"])
calls = os.path.join(work_dir, "calls.txt")
fake_tidy = os.path.join(work_dir, "fake-clang-tidy")
with open(fake_tidy, "w", encoding="utf-8") as file:
    file.write(f"""#!{sys.executable}
import os, signal, sys
if sys.argv[1:] == ["--version"]:
    print("fake clang-tidy version 1")
    sys.exit(1 if "VERSION_FAIL" in os.environ else 0)
if sys.argv[1:] == ["--verify-config"]:
    if "CONFIG_FAIL" in os.environ:
        print("warning: unknown check 'modernize-use-overide'")
        sys.exit(1)
    print("No config errors detected.")
    sys.exit(0)
if len(sys.argv) != 5 or sys.argv[1] != "-p" or os.path.realpath(sys.argv[2]) != {os.path.realpath(fake_build)!r} \
        or sys.argv[3] != "--quiet":
    print("unexpected arguments", sys.argv)
    sys.exit(3)
source = sys.argv[-1]
with open({calls!r}, "a") as log:
    log.write(source + "\\n")
name = os.path.basename(source)
if name == "Bad.cpp":
    print(source + ":1:1: error: planted finding [bugprone-planted,-warnings-as-errors]")
    sys.exit(1)
if name == "Quiet.cpp" and "QUIET_OUTPUT" in os.environ:
    print(source + os.environ["QUIET_OUTPUT"])
if name == "Crash.cpp" and "CRASH" in os.environ:
    # Killed by a signal, as a crash would be, but SIGTERM: a SIGSEGV would open macOS's crash reporter dialog.
    os.kill(os.getpid(), signal.SIGTERM)
print("12 warnings generated.")
""")
os.chmod(fake_tidy, os.stat(fake_tidy).st_mode | stat.S_IXUSR)


def checked_files():
    with open(calls, encoding="utf-8") as file:
        files = sorted(file.read().split())
    os.remove(calls)
    return files


selected = listed("-p", fake_build)
owned_sources = sorted(sources[name] for name in owned_names)
if selected != owned_sources:
    fail(f"the stand-in tree selected {selected}, not {owned_sources} (a target named like an owned one must not match)")

result = run("-p", fake_build, "-j", "2", "--clang-tidy", fake_tidy)
if result.returncode != 1 or "planted finding" not in result.stdout or "failed on 1 of 5 files" not in result.stdout:
    fail(f"a finding did not fail the run (exit {result.returncode}):\n{result.stdout}")
if "warnings generated" in result.stdout:
    fail(f"a clean file's output was printed:\n{result.stdout}")
if checked_files() != owned_sources:
    fail("not every selected file was checked")

# Findings that exit 0 (the forms real check names take) fail; the other lines a clean run can print do not.
for line in (":2:3: warning: not promoted [bugprone-planted]", ":2:3: warning: magic [cppcoreguidelines-avoid-magic-numbers,"
        "readability-magic-numbers]", ":2:3: warning: zero [clang-analyzer-core.DivideZero]",
        ":2:3: warning: extension [clang-diagnostic-c++20-extensions]", ":2:3: warning: pragma [clang-diagnostic-#warnings]"):
    result = run("-p", fake_build, "--clang-tidy", fake_tidy, sources["Quiet.cpp"], env=dict(os.environ, QUIET_OUTPUT=line))
    if result.returncode != 1 or "not errors" not in result.stdout:
        fail(f"a finding that exits 0 passed ({line}, exit {result.returncode}):\n{result.stdout}")
    checked_files()
for line in (":2:3: note: expanded from here", "\n    2 | int x = 1 / 0;\n      |         ^", "\nSuppressed 3 warnings"):
    result = run("-p", fake_build, "--clang-tidy", fake_tidy, sources["Quiet.cpp"], env=dict(os.environ, QUIET_OUTPUT=line))
    if result.returncode != 0:
        fail(f"a clean file's output failed the run ({line!r}):\n{result.stdout}")
    checked_files()
result = run("-p", fake_build, "--clang-tidy", fake_tidy, sources["Crash.cpp"], env=dict(os.environ, CRASH="1"))
if result.returncode != 1 or "Crash.cpp: clang-tidy was killed by SIGTERM" not in result.stdout:
    fail(f"a crashing clang-tidy passed (exit {result.returncode}):\n{result.stdout}")
checked_files()

# Files may be given relative to the current directory; with a shard, only those files are dealt out.
result = run("-p", "build", "--clang-tidy", fake_tidy, "Good.cpp", "Other.cpp", cwd=work_dir)
if result.returncode != 0 or "passed on 2 files" not in result.stdout:
    fail(f"a run restricted to clean files failed (exit {result.returncode}):\n{result.stdout}")
if checked_files() != sorted([sources["Good.cpp"], sources["Other.cpp"]]):
    fail("a restricted run checked other files")
if listed("-p", "build", "--shard", "2/2", "Good.cpp", "Other.cpp", cwd=work_dir) != [sources["Other.cpp"]]:
    fail("a shard of given files is not the second of the two")
rejected("has no files", "--list", "-p", fake_build, "--shard", "3/3", sources["Good.cpp"], sources["Other.cpp"])
rejected("Not translation units", "-p", fake_build, "--clang-tidy", fake_tidy, sources["Third.cpp"])

# The executable: a named one must exist (no fallback to another version), and CLANG_TIDY names one too.
rejected("was not found", "-p", fake_build, "--clang-tidy", os.path.join(work_dir, "missing"), sources["Good.cpp"])
rejected("was not found", "-p", fake_build, sources["Good.cpp"],
    env=dict(os.environ, CLANG_TIDY=os.path.join(work_dir, "missing")))
rejected("was not found", "-p", fake_build, sources["Good.cpp"], env=dict(os.environ, CLANG_TIDY=""))
result = run("-p", fake_build, sources["Good.cpp"], env=dict(os.environ, CLANG_TIDY=fake_tidy))
if result.returncode != 0 or "fake clang-tidy version 1" not in result.stdout:
    fail(f"CLANG_TIDY was not used:\n{result.stdout}")
checked_files()
# A clang-tidy that cannot report its version, or rejects .clang-tidy (a misspelled check would silently go unchecked).
rejected("--version exited with 1", "-p", fake_build, "--clang-tidy", fake_tidy, sources["Good.cpp"],
    env=dict(os.environ, VERSION_FAIL="1"))
rejected("unknown check", "-p", fake_build, "--clang-tidy", fake_tidy, sources["Good.cpp"], env=dict(os.environ, CONFIG_FAIL="1"))
if os.path.exists(calls):
    fail("files were checked although clang-tidy was unusable")

# Stale or foreign build trees: no target list, an empty one, a listed target without compile commands, and a compile
# database without object paths.
os.remove(os.path.join(fake_build, "ClangTidyTargets.txt"))
rejected("ClangTidyTargets.txt is missing", "--list", "-p", fake_build)
write_tree(commands, [])
rejected("names no targets", "--list", "-p", fake_build)
write_tree(commands, ["Owned", "Renamed"])
rejected("belongs to Renamed", "--list", "-p", fake_build)
write_tree([{key: value for key, value in item.items() if key != "output"} for item in commands], ["Owned"])
rejected("without an 'output'", "--list", "-p", fake_build)
print("clang_tidy.py checks passed")
