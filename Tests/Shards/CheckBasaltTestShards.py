#!/usr/bin/env python3
"""Checks that the ctest entries running BasaltTests (its shards, Tests/CMakeLists.txt) run every test case exactly once
(ctest BasaltTestShards): each entry's registered command lists its test cases, and together they must equal the
binary's full list, with no case in two entries and no entry empty. Each entry must also run this binary, carry the
unit label (CI selects tests by label), share the BasaltTests entry's labels, timeout, environment (the sanitizer
options) and working directory, and have no property that skips it or lets it pass without its tests passing.

Usage: CheckBasaltTestShards.py <ctest> <build dir> <BasaltTests> <entry>...
"""
import collections, json, os, subprocess, sys, tempfile
import xml.etree.ElementTree as ElementTree

ctest, build_dir, binary, *names = sys.argv[1:]
# Properties that would skip an entry, or turn a failing run into a pass.
FORBIDDEN = {"DISABLED", "WILL_FAIL", "SKIP_RETURN_CODE", "SKIP_REGULAR_EXPRESSION", "PASS_REGULAR_EXPRESSION"}
# Properties every entry takes from the BasaltTests entry.
SHARED = ("LABELS", "TIMEOUT", "ENVIRONMENT", "WORKING_DIRECTORY")


def fail(message):
    raise SystemExit(f"FAIL: {message}")


def test_cases(command, work):
    """The test cases a BasaltTests command line would run, as (file, line, name)."""
    # Appended options win over the registered --reporters=console (doctest takes an option's last occurrence); --out
    # keeps the XML apart from anything the binary logs.
    out = os.path.join(work, "cases.xml")
    result = subprocess.run([*command, "--list-test-cases", "--reporters=xml", f"--out={out}"], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        fail(f"{command} --list-test-cases exited with {result.returncode}:\n{result.stdout}{result.stderr}")
    cases = [(case.get("filename"), case.get("line"), case.get("name")) for case in ElementTree.parse(out).iter("TestCase")]
    if not cases:
        fail(f"{command} lists no test cases")
    return cases


if "BasaltTests" not in names or len(names) < 2:
    fail(f"expected the BasaltTests entry and at least one shard, got {names}")
listing = subprocess.run([ctest, "--test-dir", build_dir, "--show-only=json-v1", "-R", "^(" + "|".join(names) + ")$"], capture_output=True, text=True, check=True)
entries = {test["name"]: test for test in json.loads(listing.stdout)["tests"]}
if sorted(entries) != sorted(names):
    fail(f"ctest lists {sorted(entries)}, expected {sorted(names)}")
properties_of = {name: {item["name"]: item["value"] for item in test.get("properties", [])} for name, test in entries.items()}
for name, test in entries.items():
    properties = properties_of[name]
    differing = [key for key in SHARED if properties.get(key) != properties_of["BasaltTests"].get(key)]
    if differing:
        fail(f"{name} differs from the BasaltTests entry in {differing}: {[properties.get(key) for key in differing]}")
    if not os.path.samefile(test["command"][0], binary):
        fail(f"{name} runs {test['command'][0]}, not {binary}")
    if "unit" not in properties.get("LABELS", []):
        fail(f"{name} lacks the unit label: {properties.get('LABELS')}")
    if FORBIDDEN & properties.keys():
        fail(f"{name} has {sorted(FORBIDDEN & properties.keys())}")

with tempfile.TemporaryDirectory(prefix="BasaltTestShards-") as work:
    everything = set(test_cases([binary], work))
    runs = collections.Counter(case for test in entries.values() for case in test_cases(test["command"], work))
twice = [case for case, count in runs.items() if count > 1]
if twice:
    fail(f"{len(twice)} test cases run in more than one entry, e.g. {twice[:3]}")
missing = everything - runs.keys()
if missing:
    fail(f"{len(missing)} test cases run in no entry, e.g. {sorted(missing)[:3]}")
unknown = runs.keys() - everything
if unknown:
    fail(f"entries list test cases the full run does not: {sorted(unknown)[:3]}")
print(f"BasaltTestShards: {len(everything)} test cases over {len(entries)} entries ({', '.join(sorted(entries))}), each run once")
