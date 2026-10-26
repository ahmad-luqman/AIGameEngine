#!/usr/bin/env python3
"""Checks that the ctest entries running BasaltTests (its shards, Tests/CMakeLists.txt) run every test case exactly once
(ctest BasaltTestShards): each entry's registered command lists its test cases, and together they must equal the
binary's full list, with no case in two entries and no entry empty.

Usage: CheckBasaltTestShards.py <ctest> <build dir> <BasaltTests>
"""
import collections, json, subprocess, sys
import xml.etree.ElementTree as ElementTree

ctest, build_dir, binary = sys.argv[1:4]


def fail(message):
    raise SystemExit(f"FAIL: {message}")


def test_cases(command):
    """The test cases a BasaltTests command line would run, as (file, line, name)."""
    result = subprocess.run([*command, "--list-test-cases", "--reporters=xml"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)
    if result.returncode != 0:
        fail(f"{command} --list-test-cases exited with {result.returncode}:\n{result.stdout}{result.stderr}")
    cases = [(case.get("filename"), case.get("line"), case.get("name")) for case in ElementTree.fromstring(result.stdout).iter("TestCase")]
    if not cases:
        fail(f"{command} lists no test cases")
    return cases


listing = subprocess.run([ctest, "--test-dir", build_dir, "--show-only=json-v1", "-R", "^BasaltTests(_.+)?$"], stdout=subprocess.PIPE, text=True, check=True)
entries = {test["name"]: test["command"] for test in json.loads(listing.stdout)["tests"]}
if "BasaltTests" not in entries or len(entries) < 2:
    fail(f"expected the BasaltTests entry and at least one shard, found {sorted(entries)}")

everything = test_cases([binary])
runs = collections.Counter()
for name, command in entries.items():
    for case in test_cases(command):
        runs[case] += 1
twice = [case for case, count in runs.items() if count > 1]
if twice:
    fail(f"{len(twice)} test cases run in more than one entry, e.g. {twice[:3]}")
missing = set(everything) - set(runs)
if missing:
    fail(f"{len(missing)} test cases run in no entry, e.g. {sorted(missing)[:3]}")
unknown = set(runs) - set(everything)
if unknown:
    fail(f"entries list test cases the full run does not: {sorted(unknown)[:3]}")
print(f"BasaltTestShards: {len(everything)} test cases over {len(entries)} entries ({', '.join(sorted(entries))}), each run once")
