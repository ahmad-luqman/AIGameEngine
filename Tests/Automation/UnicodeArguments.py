"""Passes command-line arguments outside every single-byte code page to the basalt CLI and the runtime (ctest
UnicodeArguments).

Windows hands a program's narrow argv over in the ANSI code page (1252 on CI), which has no CJK characters: the CLI and
the runtime read the wide command line instead (Platform::GetProcessArguments). Python starts them with the arguments
as UTF-16, so this checks what a user's shell would pass: a project folder, JSON parameters (raw UTF-8, not \\u
escapes), a batch file and a scene, all named in CJK characters.

Usage: UnicodeArguments.py <basalt CLI> [<BasaltRuntime>]
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def fail(message):
    print(f"FAIL: {message}")
    sys.exit(1)


def run(arguments, env):
    """The process's stdout (JSON responses) and stderr (logs), after checking it succeeded."""
    # UTF-8 whatever the console's code page: the CLI and the runtime write responses and logs as UTF-8.
    result = subprocess.run([str(argument) for argument in arguments], capture_output=True, encoding="utf-8", errors="replace", timeout=120, env=env)
    if result.returncode != 0:
        fail(f"{' '.join(str(argument) for argument in arguments)} exited {result.returncode}:\n{result.stdout}{result.stderr}")
    return result.stdout, result.stderr


def response(text):
    start = text.find("{")
    if start < 0:
        fail(f"no JSON response in:\n{text}")
    return json.loads(text[start:])


def main():
    cli = Path(sys.argv[1]).resolve()
    runtime = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
    with tempfile.TemporaryDirectory(prefix="BasaltUnicode-") as temp:
        root = Path(temp)
        env = dict(os.environ, BASALT_USER_DATA_DIR=str(root / "UserData"))
        # CJK ideographs only: no characters that Unicode normalization changes (macOS file systems compare normalized).
        project = root / "資産 日本語"
        created = response(run([cli, "--no-save", "project.create", json.dumps({"path": str(project), "name": "日本語"}, ensure_ascii=False)], env)[0])
        if not project.is_dir() or Path(created["result"]["directory"]).resolve() != project.resolve():
            fail(f"project.create made {created}, not {project}")

        batch = root / "手順.batch.json"
        batch.write_text(json.dumps([
            {"command": "scene.new", "params": {"name": "場面"}},
            {"command": "entity.create", "params": {"name": "主役"}},
            {"command": "scene.save", "params": {"path": "Assets/Scenes/場面.bscene"}},
            {"command": "project.set", "params": {"startScene": "Assets/Scenes/場面.bscene"}},
        ], ensure_ascii=False), encoding="utf-8")
        run([cli, "--project", project, "batch", batch], env)
        if not (project / "Assets" / "Scenes" / "場面.bscene").is_file():
            fail("the batch did not save Assets/Scenes/場面.bscene")

        info = response(run([cli, "--project", project, "--scene", "Assets/Scenes/場面.bscene", "--no-save", "scene.info"], env)[0])["result"]
        if info.get("path") != "Assets/Scenes/場面.bscene" or info.get("name") != "場面":
            fail(f"scene.info after --scene reported {info}")
        project_info = response(run([cli, "--project", project, "--no-save", "project.info"], env)[0])["result"]
        if project_info.get("name") != "日本語" or project_info.get("startScene") != "Assets/Scenes/場面.bscene":
            fail(f"project.info reported {project_info}")

        if runtime:
            log = run([runtime, "--project", project, "--scene", "Assets/Scenes/場面.bscene", "--headless", "--frames", "3"], env)[1]
            if "Runtime: playing 'Assets/Scenes/場面.bscene'" not in log:
                fail(f"the runtime did not play the scene:\n{log}")
    print("PASS")


if __name__ == "__main__":
    main()
