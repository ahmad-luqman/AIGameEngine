"""Crashes a process on purpose and checks its crash report (ctest CrashReport).

Runs `BasaltTests --basalt-child crash-report <kind>` (Tests/Source/TestMain.cpp), which installs the crash reporter,
logs a sentinel line and crashes: an invalid memory access, abort(), an uncaught C++ exception (a std::exception and an
int), a pure virtual call, an invalid C runtime parameter (Windows), a trap, stack overflows on the installing thread, on
a thread holding a ThreadStack and on a job worker, and invalid memory accesses on worker threads. The child installs
the reporter, then calls Install again from two threads at once, and fails if the calls disagree. Each run must die by
its signal on macOS and Linux (the reporter re-raises it), or with the exception code or the C runtime's abort code 3 on Windows, and leave exactly one report in
<user data>/CrashChild/Crashes naming the reason, with a stack trace, the context line and the last log lines; Windows
reports add a minidump. More runs check the report folders: a user data directory whose ancestors do not exist yet (they
are created, up to eight missing levels), one that cannot hold folders at all, and (macOS, Linux) one whose report
folder path is too long for the handler's buffers while its parent fits; the last two put the report in CrashReports
next to the executable. On Linux with addr2line, reports must name the executable's frames. On macOS and Linux, stand-ins
for the dialog program and opener check what the dialogs run, through a crash and through CrashReporter::ShowErrorDialog
(Windows' message boxes cannot be answered here, but a run with dialogs off must not wait on one); on macOS osacompile
checks the AppleScript. Crashing children use a stand-in dialog too, so a broken check cannot open real alerts.
Exits 77 (skipped) when crash reports are unavailable (AddressSanitizer builds).
"""
import os
import pathlib
import re
import stat
import subprocess
import sys
import tempfile
import time

WINDOWS = sys.platform == "win32"
LINUX = sys.platform.startswith("linux")
# The kinds whose addr2line finished in time.
SOURCE_LINE_RUNS = []

# Kind -> the reasons a report may name (the same fault can arrive as either signal, depending on CPU and OS).
REASONS = {
    "segv": ["EXCEPTION_ACCESS_VIOLATION"] if WINDOWS else ["SIGSEGV", "SIGBUS"],
    "abort": ["SIGABRT"],
    "terminate": ["SIGABRT"],
    "throw-int": ["SIGABRT"],
    "purecall": ["a pure virtual function call"] if WINDOWS else ["SIGABRT"],
    "trap": ["EXCEPTION_BREAKPOINT"] if WINDOWS else ["SIGTRAP", "SIGILL"],
    "stackoverflow": ["EXCEPTION_STACK_OVERFLOW"] if WINDOWS else ["SIGSEGV", "SIGBUS"],
    "thread-segv": ["EXCEPTION_ACCESS_VIOLATION"] if WINDOWS else ["SIGSEGV", "SIGBUS"],
    "nested-stackoverflow": ["EXCEPTION_STACK_OVERFLOW"] if WINDOWS else ["SIGSEGV", "SIGBUS"],
    "thread-stackoverflow": ["EXCEPTION_STACK_OVERFLOW"] if WINDOWS else ["SIGSEGV", "SIGBUS"],
    "job-stackoverflow": ["EXCEPTION_STACK_OVERFLOW"] if WINDOWS else ["SIGSEGV", "SIGBUS"],
    "two-thread-segv": ["EXCEPTION_ACCESS_VIOLATION"] if WINDOWS else ["SIGSEGV", "SIGBUS"],
}
if WINDOWS:
    REASONS["invalid-parameter"] = ["an invalid parameter passed to a C runtime function"]
# Kinds that end through abort() or a C runtime failure: exit code 3 on Windows.
ABORTING = ("abort", "terminate", "throw-int", "purecall", "invalid-parameter")


def fail(message):
    print(f"FAIL: {message}")
    sys.exit(1)


def run_child(tests, arguments, user_data, timeout=120, **options):
    environment = dict(os.environ, BASALT_USER_DATA_DIR=str(user_data))
    environment.pop("BASALT_TEST_CHILD_ACTIONS", None)
    run = subprocess.run([str(tests), "--basalt-child", *arguments], env=environment, capture_output=True, text=True, errors="replace", timeout=timeout, **options)
    if run.returncode == 77:
        print(run.stdout.strip())
        sys.exit(77)
    return run


def crash(tests, kind, user_data):
    run = run_child(tests, ["crash-report", kind], user_data)
    if run.returncode == 0:
        fail(f"{kind}: the child exited cleanly")
    if not WINDOWS and run.returncode > 0:
        fail(f"{kind}: exit code {run.returncode}, not death by a signal (the reporter must re-raise it); stderr: {run.stderr}")
    if WINDOWS and kind in ABORTING and run.returncode != 3:
        fail(f"{kind}: exit code {run.returncode}, not abort's 3; stderr: {run.stderr}")
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
    if kind == "throw-int" and "Uncaught exception: an exception that is not a std::exception" not in report:
        fail(f"{kind}: the uncaught exception is not named:\n{report}")
    if "CrashChild crashed" not in run.stderr or reports[0].name not in run.stderr or "incomplete" in run.stderr:
        fail(f"{kind}: stderr does not point at a complete report: {run.stderr}")
    if LINUX and pathlib.Path("/usr/bin/addr2line").exists():
        check_source_lines(kind, report)
    if WINDOWS:
        dump = reports[0].with_suffix(".dmp")
        if not dump.is_file() or dump.stat().st_size == 0 or "Minidump: " not in report:
            fail(f"{kind}: no minidump next to the report")
    print(f"{kind}: {reason}, {len(frames)} frames")


def check_source_lines(kind, report):
    _, heading, section = report.partition("\nSource lines (addr2line):\n")
    if not heading:
        fail(f"{kind}: no source lines:\n{report}")
    # A debug binary can outlast addr2line's limit on a loaded machine: some run must still finish.
    if "did not finish" in section:
        print(f"{kind}: addr2line ran out of time")
        return
    SOURCE_LINE_RUNS.append(kind)
    # "0x...: function at file:line", or "?? ??:0" for a frame addr2line cannot name.
    named = [line for line in section.splitlines() if re.match(r"\s*(0x[0-9a-f]+: )?(?!\?\?)\S", line) and "failed" not in line]
    if not named:
        fail(f"{kind}: addr2line named no frame:\n{section}")
    if kind == "segv" and "CrashNow" not in section:
        fail(f"{kind}: the crashing function is not named:\n{section}")


def write_program(path, script):
    path.write_text("#!/bin/sh\n" + script)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def wait_for(path, what):
    deadline = time.monotonic() + 30
    while not path.exists() or path.stat().st_size == 0:
        if time.monotonic() > deadline:
            fail(f"{what}: nothing appeared at {path}")
        time.sleep(0.02)
    return path.read_text()


def run_dialog(tests, kind, mode, answer, folder, hold=None, opener=True, program=None):
    """Runs a child with stand-in tools: the dialog logs its arguments (waiting first until the file hold exists) and answers with exit code answer; the opener logs what it opens."""
    log, opened = folder / "dialog.log", folder / "opened.log"
    # Written whole and moved into place, so that a reader never sees part of it; appended, as a run may show two.
    wait = f'tries=0\nwhile [ ! -e "{hold}" ] && [ $tries -lt 2600 ]; do sleep 0.05; tries=$((tries + 1)); done\n' if hold else ""
    write_program(folder / "dialog", f'{wait}cat "{log}" > "{log}.$$" 2>/dev/null\nfor argument in "$@"; do printf "%s\\n" "$argument"; done >> "{log}.$$"\nmv "{log}.$$" "{log}"\nexit {answer}\n')
    write_program(folder / "opener", f'printf "%s" "$1" > "{opened}.$$"\nmv "{opened}.$$" "{opened}"\n')
    opener_path = opener if isinstance(opener, str) else str(folder / "opener") if opener else ""
    # A quote, a space and a command substitution in the path: it reaches the tools only as an argument.
    user_data = folder / "it's $(touch injected) here"
    run = run_child(tests, ["crash-dialog", kind, program or str(folder / "dialog"), opener_path, mode], user_data, cwd=folder)
    if mode == "crash" and run.returncode >= 0:
        fail(f"{kind}: exit code {run.returncode}, not death by a signal; stderr: {run.stderr}")
    if mode != "crash" and run.returncode != 0:
        fail(f"{kind} {mode}: exit code {run.returncode}; stderr: {run.stderr}")
    return run, log, opened, user_data


def check_dialogs(tests):
    expected_labels = {"zenity": "--ok-label", "kdialog": "--yes-label", "xmessage": "-buttons"}
    for kind, label in expected_labels.items():
        # Choosing "Open Report" opens exactly the report.
        with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
            folder = pathlib.Path(directory)
            run, log, opened, user_data = run_dialog(tests, kind, "crash", 0, folder)
            arguments = wait_for(log, f"{kind} dialog")
            report = wait_for(opened, f"{kind} opener")
            reports = sorted((user_data / "CrashChild" / "Crashes").glob("crash-*.txt"))
            if len(reports) != 1 or report != str(reports[0]):
                fail(f"{kind}: opened {report!r}, reports are {reports}; stderr: {run.stderr}")
            if label not in arguments or "CrashChild crashed" not in arguments or report not in arguments:
                fail(f"{kind}: unexpected dialog arguments:\n{arguments}")
            if (folder / "injected").exists():
                fail(f"{kind}: text from the report's path ran as a command")

        # Closing the dialog opens nothing.
        with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
            folder = pathlib.Path(directory)
            run, log, opened, _ = run_dialog(tests, kind, "crash", 1, folder)
            wait_for(log, f"closed {kind} dialog")
            # Starting a new script can take a while on macOS.
            time.sleep(1)
            if opened.exists():
                fail(f"closing the {kind} dialog opened the report")

        # A failed start: the plain error dialog, with no report to offer.
        with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
            folder = pathlib.Path(directory)
            run, log, opened, _ = run_dialog(tests, kind, "error", 0, folder)
            arguments = wait_for(log, f"{kind} error dialog").splitlines()
            if 'The "archive" is broken: $(touch injected)' not in arguments or "second line" not in arguments or label in arguments:
                fail(f"unexpected {kind} error dialog arguments: {arguments}")
            if kind != "xmessage" and "CrashChild" not in arguments:
                fail(f"the {kind} error dialog has no title: {arguments}")
            if opened.exists() or (folder / "injected").exists():
                fail(f"the {kind} error dialog ran something it should not")

    # Without an opener the dialog only shows the path.
    with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
        folder = pathlib.Path(directory)
        run, log, opened, user_data = run_dialog(tests, "zenity", "crash", 0, folder, opener=False)
        arguments = wait_for(log, "dialog without an opener")
        reports = sorted((user_data / "CrashChild" / "Crashes").glob("crash-*.txt"))
        if "--error\n" not in arguments or not reports or str(reports[0]) not in arguments:
            fail(f"unexpected dialog without an opener: {arguments}")

    # An opener that cannot run: the same, rather than a button that does nothing.
    with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
        folder = pathlib.Path(directory)
        (folder / "not-a-program").write_text("text")
        run, log, opened, _ = run_dialog(tests, "zenity", "crash", 0, folder, opener=str(folder / "not-a-program"))
        arguments = wait_for(log, "dialog with a bad opener")
        if "--error\n" not in arguments or opened.exists():
            fail(f"unexpected dialog with a bad opener: {arguments}")

    # A dialog program refused at Install, or (a folder passes that check) that cannot start, costs nothing but the dialog.
    with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
        folder = pathlib.Path(directory)
        run, _, _, user_data = run_dialog(tests, "zenity", "crash", 0, folder, program=str(folder / "missing"))
        check_one_report(run, user_data, "missing dialog program")
    with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
        folder = pathlib.Path(directory)
        run, _, _, user_data = run_dialog(tests, "zenity", "crash", 0, folder, opener=False, program=directory)
        check_one_report(run, user_data, "dialog program that cannot start")
        if "could not be shown" not in run.stderr:
            fail(f"no word of the dialog that could not start: {run.stderr}")
    with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
        run, _, _, _ = run_dialog(tests, "zenity", "error", 0, pathlib.Path(directory), opener=False, program=directory)
        if "could not start" not in run.stdout + run.stderr:
            fail(f"no word of the error dialog that could not start: {run.stdout}{run.stderr}")

    # The crashed process does not wait for the dialog, and leaves the test's pipes alone: subprocess.run returns while the
    # dialog is held, or the run times out.
    with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
        folder = pathlib.Path(directory)
        run, log, _, user_data = run_dialog(tests, "zenity", "crash", 1, folder, hold=folder / "released")
        (folder / "released").touch()
        check_one_report(run, user_data, "held dialog")
        wait_for(log, "held dialog")

    # The macOS kind, through a stand-in: never /usr/bin/osascript, which would show it.
    with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
        folder = pathlib.Path(directory)
        run, log, _, _ = run_dialog(tests, "osascript", "crash", 0, folder)
        arguments = wait_for(log, "osascript dialog")
        if "-e\n" not in arguments or "display alert" not in arguments or "Open Report" not in arguments:
            fail(f"unexpected osascript arguments:\n{arguments}")

    # A message too long for any buffer still gives a whole script.
    with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
        folder = pathlib.Path(directory)
        run, log, _, _ = run_dialog(tests, "osascript", "long", 0, folder)
        script = wait_for(log, "long osascript dialog").split("-e\n", 1)[1].rstrip("\n")
        if not script.endswith('as critical buttons {"OK"}'):
            fail(f"a long message broke the script: ...{script[-200:]}")
        if sys.platform == "darwin":
            compile_apple_script(script, folder / "long.scpt")

    # Dialogs stay off unless the host turns them on.
    with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
        folder = pathlib.Path(directory)
        run, log, _, _ = run_dialog(tests, "zenity", "disabled", 0, folder)
        arguments = wait_for(log, "dialog after turning dialogs on")
        if "dialogs on" not in arguments or "no dialog" in arguments:
            fail(f"a dialog showed while dialogs were off:\n{arguments}")
    print("dialogs: open, close, error, fallbacks and off")


def check_one_report(run, user_data, what):
    reports = sorted((user_data / "CrashChild" / "Crashes").glob("crash-*.txt"))
    if len(reports) != 1 or "Last log lines" not in reports[0].read_text(encoding="utf-8", errors="replace"):
        fail(f"{what}: no whole report in {reports}; stderr: {run.stderr}")


def check_dialogs_off(tests):
    # On Windows too: a message box here would hang until this timeout, as it would hang any automated run.
    with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
        try:
            run = run_child(tests, ["crash-dialog", "zenity", "unused", "unused", "off"], directory, timeout=60)
        except subprocess.TimeoutExpired:
            fail("a dialog is waiting while dialogs are off")
        if run.returncode != 0:
            fail(f"dialogs off: exit code {run.returncode}; stderr: {run.stderr}")


def compile_apple_script(script, output):
    compiled = subprocess.run(["osacompile", "-o", str(output), "-e", script], capture_output=True, text=True, timeout=60)
    if compiled.returncode != 0:
        fail(f"AppleScript does not compile: {compiled.stderr}\n{script[:500]}")


def check_apple_script(tests):
    if sys.platform != "darwin":
        return
    with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
        for name, message, report in (("alert", 'Back\\slash and "quotes"', ""), ("offer", 'Back\\slash and "quotes"', '/tmp/it\'s a "quoted" \\ path.txt'), ("lines", "First line.\n\nThird line.", "/tmp/report.txt")):
            run = subprocess.run([str(tests), "--basalt-child", "crash-script", 'App "One"', message, report], capture_output=True, text=True, timeout=60)
            if run.returncode != 0 or ("Open Report" in run.stdout) != bool(report):
                fail(f"AppleScript {name}: exit code {run.returncode}: {run.stdout}")
            compile_apple_script(run.stdout, pathlib.Path(directory, name + ".scpt"))
    print("AppleScript compiles")


def main():
    tests = pathlib.Path(sys.argv[1]).resolve()
    for kind in REASONS:
        # Two threads crashing at once race; a few runs make their overlap likely.
        for _ in range(4 if kind == "two-thread-segv" else 1):
            with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
                run = crash(tests, kind, pathlib.Path(directory))
                check_report(kind, run, pathlib.Path(directory, "CrashChild", "Crashes"))

    # Folders are created only when something crashes, several levels if need be (a fresh account without ~/.local, a
    # nested BASALT_USER_DATA_DIR).
    with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
        nested = pathlib.Path(directory, "not", "yet", "there")
        run = crash(tests, "segv", nested)
        check_report("segv", run, nested / "CrashChild" / "Crashes")

    # A user data directory that cannot hold folders (a file): the report goes next to the executable.
    with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
        blocked = pathlib.Path(directory, "UserDataIsAFile")
        blocked.write_text("not a folder")
        check_fallback(tests, "fallback (user data is a file)", blocked)

    # A report folder too long for the handler's 1024-byte buffers while its parents fit: the report must not land in a
    # parent folder, but next to the executable (the user data folder itself stays short enough to create).
    if not WINDOWS:
        with tempfile.TemporaryDirectory(prefix="BasaltCrash-") as directory:
            target = 1010 - len(str(pathlib.Path(directory)))
            long_folder = pathlib.Path(directory)
            while target > 1:
                part = "d" * min(200, target - 1)
                long_folder /= part
                target -= len(part) + 1
            long_folder.mkdir(parents=True)
            if len(str(long_folder / "CrashChild" / "Crashes")) <= 1024 or len(str(long_folder / "CrashChild")) > 1024:
                fail(f"long path setup: {len(str(long_folder))} characters")
            check_fallback(tests, "fallback (report folder too long)", long_folder)
            if any(long_folder.rglob("crash-*")):
                fail("long path: a report landed under the user data folder")
    if LINUX and pathlib.Path("/usr/bin/addr2line").exists() and not SOURCE_LINE_RUNS:
        fail("addr2line ran out of time on every run")
    check_dialogs_off(tests)
    if not WINDOWS:
        check_dialogs(tests)
    check_apple_script(tests)
    print("crash reports passed")


def check_fallback(tests, label, user_data):
    fallback = tests.parent / "CrashReports"
    before = set(fallback.glob("crash-*")) if fallback.is_dir() else set()
    run = crash(tests, "segv", user_data)
    created = sorted(set(fallback.glob("crash-*")) - before)
    try:
        text_reports = [path for path in created if path.suffix == ".txt"]
        if len(text_reports) != 1:
            fail(f"{label}: expected one new report in {fallback}, found {created}; stderr: {run.stderr}")
        report = text_reports[0].read_text(encoding="utf-8", errors="replace")
        if "Application: CrashChild" not in report or text_reports[0].name not in run.stderr:
            fail(f"{label}: unexpected report or stderr: {run.stderr}\n{report}")
        print(f"{label}: {text_reports[0]}")
    finally:
        for path in created:
            path.unlink()


if __name__ == "__main__":
    main()
