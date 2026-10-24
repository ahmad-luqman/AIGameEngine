"""Crashes a process on purpose and checks its crash report (ctest CrashReport).

Runs `BasaltTests --basalt-child crash-report <kind>` (Tests/Source/TestMain.cpp), which installs the crash reporter,
logs a sentinel line and crashes, once per kind: an invalid memory access, abort(), an uncaught C++ exception and a
trap. Each run must die as it would have without the reporter (a signal on macOS and Linux, the exception code or
abort's exit code 3 on Windows) and leave exactly one report in <user data>/CrashChild/Crashes naming the reason, with a
stack trace, the context line and the last log lines; Windows reports add a minidump. Exits 77 (skipped) when crash
reports are unavailable (AddressSanitizer builds).
"""
import os
import pathlib
import subprocess
import sys
import tempfile

WINDOWS = sys.platform == "win32"

# Kind -> the reasons a report may name (the same fault can arrive as either signal, depending on CPU and OS).
REASONS = {
    "segv": ["EXCEPTION_ACCESS_VIOLATION"] if WINDOWS else ["SIGSEGV", "SIGBUS"],
    "abort": ["SIGABRT"],
    "terminate": ["SIGABRT"],
    "trap": ["EXCEPTION_BREAKPOINT"] if WINDOWS else ["SIGTRAP", "SIGILL"],
}


def fail(message):
    print(f"FAIL: {message}")
    sys.exit(1)


def check(kind, tests, root):
    environment = dict(os.environ, BASALT_USER_DATA_DIR=str(root))
    environment.pop("BASALT_TEST_CHILD_ACTIONS", None)
    run = subprocess.run([tests, "--basalt-child", "crash-report", kind], env=environment, capture_output=True, text=True, errors="replace", timeout=120)
    if run.returncode == 77:
        print(run.stdout.strip())
        sys.exit(77)
    if run.returncode == 0:
        fail(f"{kind}: the child exited cleanly")
    if not WINDOWS and run.returncode > 0:
        fail(f"{kind}: exit code {run.returncode}, not death by a signal (the reporter must re-raise it)")
    if WINDOWS and kind in ("abort", "terminate") and run.returncode != 3:
        fail(f"{kind}: exit code {run.returncode}, not abort's 3")

    reports = sorted((root / "CrashChild" / "Crashes").glob("crash-*.txt"))
    if len(reports) != 1:
        fail(f"{kind}: expected one report, found {[str(r) for r in reports]}; stderr: {run.stderr}")
    report = reports[0].read_text(encoding="utf-8", errors="replace")
    for expected in ("Basalt crash report\n", "Application: CrashChild\n", "Project: Crash Test 1.2.3\n", "\nStack trace:\n", "\nLast log lines (oldest first):\n", "crash-test sentinel 42"):
        if expected not in report:
            fail(f"{kind}: report lacks {expected!r}:\n{report}")
    reason = next((line for line in report.splitlines() if line.startswith("Reason: ")), "")
    if not any(name in reason for name in REASONS[kind]):
        fail(f"{kind}: reason {reason!r} names none of {REASONS[kind]}")
    trace = report.split("\nStack trace:\n", 1)[1].split("\nLast log lines", 1)[0]
    frames = [line for line in trace.splitlines() if line.strip()]
    if len(frames) < 3:
        fail(f"{kind}: stack trace has {len(frames)} frames:\n{trace}")
    if kind == "terminate" and "Uncaught exception: crash-test exception" not in report:
        fail(f"{kind}: the uncaught exception's message is missing:\n{report}")
    if f"CrashChild crashed" not in run.stderr or reports[0].name not in run.stderr:
        fail(f"{kind}: stderr does not point at the report: {run.stderr}")
    if WINDOWS:
        dump = reports[0].with_suffix(".dmp")
        if not dump.is_file() or dump.stat().st_size == 0 or f"Minidump: " not in report:
            fail(f"{kind}: no minidump next to the report")
    print(f"{kind}: {reason}, {len(frames)} frames")


def main():
    tests = sys.argv[1]
    for kind in REASONS:
        with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
            check(kind, tests, pathlib.Path(directory))
    print("crash reports passed")


if __name__ == "__main__":
    main()
