"""Crashes a process on purpose and checks its crash report (ctest CrashReport).

Runs `BasaltTests --basalt-child crash-report <kind>` (Tests/Source/TestMain.cpp), which installs the crash reporter,
logs a sentinel line and crashes: an invalid memory access, abort(), an uncaught C++ exception, a trap, a stack overflow
and an invalid memory access on a worker thread. Each run must die as it would have without the reporter (a signal on
macOS and Linux, the exception code or abort's exit code 3 on Windows) and leave exactly one report in
<user data>/CrashChild/Crashes naming the reason, with a stack trace, the context line and the last log lines; Windows
reports add a minidump. Two more runs check the report folders: a user data directory whose ancestors do not exist yet
(they are created), and one that cannot hold folders at all (the report goes to CrashReports next to the executable).
Exits 77 (skipped) when crash reports are unavailable (AddressSanitizer builds).
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
    "stackoverflow": ["EXCEPTION_STACK_OVERFLOW"] if WINDOWS else ["SIGSEGV", "SIGBUS"],
    "thread-segv": ["EXCEPTION_ACCESS_VIOLATION"] if WINDOWS else ["SIGSEGV", "SIGBUS"],
}


def fail(message):
    print(f"FAIL: {message}")
    sys.exit(1)


def crash(tests, kind, user_data):
    environment = dict(os.environ, BASALT_USER_DATA_DIR=str(user_data))
    environment.pop("BASALT_TEST_CHILD_ACTIONS", None)
    run = subprocess.run([str(tests), "--basalt-child", "crash-report", kind], env=environment, capture_output=True, text=True, errors="replace", timeout=120)
    if run.returncode == 77:
        print(run.stdout.strip())
        sys.exit(77)
    if run.returncode == 0:
        fail(f"{kind}: the child exited cleanly")
    if not WINDOWS and run.returncode > 0:
        fail(f"{kind}: exit code {run.returncode}, not death by a signal (the reporter must re-raise it); stderr: {run.stderr}")
    if WINDOWS and kind in ("abort", "terminate") and run.returncode != 3:
        fail(f"{kind}: exit code {run.returncode}, not abort's 3")
    return run


def check_report(kind, run, folder):
    reports = sorted(folder.glob("crash-*.txt"))
    if len(reports) != 1:
        fail(f"{kind}: expected one report in {folder}, found {[str(r) for r in reports]}; stderr: {run.stderr}")
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
    if "CrashChild crashed" not in run.stderr or reports[0].name not in run.stderr or "incomplete" in run.stderr:
        fail(f"{kind}: stderr does not point at a complete report: {run.stderr}")
    if WINDOWS:
        dump = reports[0].with_suffix(".dmp")
        if not dump.is_file() or dump.stat().st_size == 0 or "Minidump: " not in report:
            fail(f"{kind}: no minidump next to the report")
    print(f"{kind}: {reason}, {len(frames)} frames")


def main():
    tests = pathlib.Path(sys.argv[1]).resolve()
    for kind in REASONS:
        with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
            run = crash(tests, kind, pathlib.Path(directory))
            check_report(kind, run, pathlib.Path(directory, "CrashChild", "Crashes"))

    # Folders are created only when something crashes, however many are missing (a fresh account without ~/.local, a
    # nested BASALT_USER_DATA_DIR).
    with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
        nested = pathlib.Path(directory, "not", "yet", "there")
        run = crash(tests, "segv", nested)
        check_report("segv", run, nested / "CrashChild" / "Crashes")

    # A user data directory that cannot hold folders (a file): the report goes next to the executable.
    with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
        blocked = pathlib.Path(directory, "UserDataIsAFile")
        blocked.write_text("not a folder")
        fallback = tests.parent / "CrashReports"
        before = set(fallback.glob("crash-*")) if fallback.is_dir() else set()
        run = crash(tests, "segv", blocked)
        created = sorted(set(fallback.glob("crash-*")) - before)
        try:
            text_reports = [path for path in created if path.suffix == ".txt"]
            if len(text_reports) != 1:
                fail(f"fallback: expected one new report in {fallback}, found {created}; stderr: {run.stderr}")
            report = text_reports[0].read_text(encoding="utf-8", errors="replace")
            if "Application: CrashChild" not in report or text_reports[0].name not in run.stderr:
                fail(f"fallback: unexpected report or stderr: {run.stderr}\n{report}")
            print(f"fallback: {text_reports[0]}")
        finally:
            for path in created:
                path.unlink()
    print("crash reports passed")


if __name__ == "__main__":
    main()
