#!/usr/bin/env python3
"""Checks that the ctest entries running BasaltTests (its shards, Tests/CMakeLists.txt) run every test case exactly once
(ctest BasaltTestShards): each entry's registered command lists its test cases, and together they must equal the
binary's full list, with no case in two entries and no entry empty. The entries must be every ctest entry that runs this
binary, differ from the BasaltTests entry only in their file filter, and share its properties (labels, which CI selects
tests by, the timeout, the environment with the sanitizer options, the working directory); none may have a property
that skips it or lets it pass without its tests passing. A run whose filters select nothing must fail.

Usage: CheckBasaltTestShards.py <ctest> <build dir> <BasaltTests> <entry>...
"""
import collections, json, os, subprocess, sys, tempfile
import xml.etree.ElementTree as ElementTree

ctest, build_dir, binary, *names = sys.argv[1:]
# Properties that would skip an entry, or turn a failing run into a pass.
FORBIDDEN = {"DISABLED", "WILL_FAIL", "SKIP_RETURN_CODE", "SKIP_REGULAR_EXPRESSION", "PASS_REGULAR_EXPRESSION"}
# Properties every entry must share with the BasaltTests entry.
SHARED = ("LABELS", "TIMEOUT", "ENVIRONMENT", "ENVIRONMENT_MODIFICATION", "FAIL_REGULAR_EXPRESSION", "WORKING_DIRECTORY")
FILTERS = ("--source-file=", "--source-file-exclude=")


def fail(message):
    raise SystemExit(f"FAIL: {message}")


def runs_binary(test):
    return bool(test.get("command")) and os.path.exists(test["command"][0]) and os.path.samefile(test["command"][0], binary)


def test_cases(name, command, work):
    """The test cases a BasaltTests command line would run, as (file, line, name)."""
    # Appended options win over the registered --reporters=console (doctest takes an option's last occurrence); --out
    # keeps the XML apart from anything the binary logs, in a file of this entry's own.
    out = os.path.join(work, f"{name}.xml")
    result = subprocess.run([*command, "--list-test-cases", "--reporters=xml", f"--out={out}"], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        fail(f"{command} --list-test-cases exited with {result.returncode}:\n{result.stdout}{result.stderr}")
    cases = [(case.get("filename"), case.get("line"), case.get("name")) for case in ElementTree.parse(out).iter("TestCase")]
    if not cases:
        fail(f"{command} lists no test cases")
    return cases


if "BasaltTests" not in names or len(names) < 2:
    fail(f"expected the BasaltTests entry and at least one shard, got {names}")
listing = subprocess.run([ctest, "--test-dir", build_dir, "--show-only=json-v1"], capture_output=True, text=True, check=False)
if listing.returncode != 0:
    fail(f"ctest --show-only exited with {listing.returncode}:\n{listing.stdout}{listing.stderr}")
entries = {test["name"]: test for test in json.loads(listing.stdout)["tests"] if runs_binary(test)}
if sorted(entries) != sorted(names):
    fail(f"the ctest entries running {binary} are {sorted(entries)}, expected {sorted(names)}")

properties_of = {name: {item["name"]: item["value"] for item in test.get("properties", [])} for name, test in entries.items()}
reference = properties_of["BasaltTests"]
if "unit" not in reference.get("LABELS", []):
    fail(f"BasaltTests lacks the unit label: {reference.get('LABELS')}")
reference_arguments = [argument for argument in entries["BasaltTests"]["command"][1:] if not argument.startswith(FILTERS)]
for name, test in entries.items():
    properties = properties_of[name]
    differing = [key for key in SHARED if properties.get(key) != reference.get(key)]
    if differing:
        fail(f"{name} differs from the BasaltTests entry in {differing}: {[properties.get(key) for key in differing]}")
    forbidden = sorted(FORBIDDEN & properties.keys())
    if forbidden:
        fail(f"{name} has {forbidden}")
    arguments = [argument for argument in test["command"][1:] if not argument.startswith(FILTERS)]
    if arguments != reference_arguments:
        fail(f"{name} runs with {arguments}, the BasaltTests entry with {reference_arguments}")

with tempfile.TemporaryDirectory(prefix="BasaltTestShards-") as work:
    everything = set(test_cases("all", [binary], work))
    runs = collections.Counter(case for name, test in entries.items() for case in test_cases(name, test["command"], work))
twice = [case for case, count in runs.items() if count > 1]
if twice:
    fail(f"{len(twice)} test cases run in more than one entry, e.g. {twice[:3]}")
missing = everything - runs.keys()
if missing:
    fail(f"{len(missing)} test cases run in no entry, e.g. {sorted(missing)[:3]}")
unknown = runs.keys() - everything
if unknown:
    fail(f"entries list test cases the full run does not: {sorted(unknown)[:3]}")

# An entry whose filter stopped matching must fail rather than pass with nothing run (TestMain.cpp).
empty = subprocess.run([binary, "--no-intro=true", "--source-file=*NoSuchTestFile.cpp"], capture_output=True, text=True, check=False)
if empty.returncode != 1 or "selected no test case" not in empty.stdout + empty.stderr:
    fail(f"a run that selects no test case exited with {empty.returncode}:\n{empty.stdout}{empty.stderr}")
print(f"BasaltTestShards: {len(everything)} test cases over {len(entries)} entries ({', '.join(sorted(entries))}), each run once")
