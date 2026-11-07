"""The hosts that edit a game stop a script at ScriptInstructionLimit; the runtime, which plays one, never does (ctest
ScriptInstructionLimit). One script, over the limit but finite, fails in the CLI and finishes in BasaltRuntime, also
once exported. An endless one then fails in the CLI, which must answer rather than hang: the timeouts are the check.

Usage: ScriptInstructionLimit.py <basalt CLI> <BasaltRuntime>
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

TIMEOUT = 120


def fail(message):
    print(f"FAIL: {message}")
    sys.exit(1)


def main():
    cli, runtime = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory() as temp:
        project = Path(temp) / "Limited"
        env = {"BASALT_USER_DATA_DIR": str(Path(temp) / "User")}

        def run(*arguments):
            return subprocess.run(arguments, capture_output=True, text=True, timeout=TIMEOUT, env={**os.environ, **env})

        def batch(requests):
            path = Path(temp) / "batch.json"
            path.write_text(json.dumps(requests), encoding="utf-8")
            return run(str(cli), "--project", str(project), "batch", str(path))

        created = run(str(cli), "project.create", json.dumps({"path": str(project), "name": "Limited"}))
        if created.returncode != 0:
            fail(f"project.create: {created.stdout}{created.stderr}")
        script = project / "Assets" / "Scripts" / "Long.lua"
        script.parent.mkdir(parents=True, exist_ok=True)
        script.write_text("local Long = {}\nfunction Long:OnUpdate(dt)\n\tfor i = 1, 2000000 do end\nend\nreturn Long\n", encoding="utf-8")
        built = batch([
            {"command": "scene.new", "params": {"name": "Main"}},
            {"command": "entity.create", "params": {"name": "Busy", "components": {"Script": {"Script": "Assets/Scripts/Long.lua"}}}},
            {"command": "scene.save", "params": {"path": "Assets/Scenes/Main.bscene"}},
            {"command": "project.set", "params": {"startScene": "Assets/Scenes/Main.bscene", "scriptInstructionLimit": 100000}},
        ])
        if built.returncode != 0:
            fail(f"building the scene: {built.stdout}{built.stderr}")

        played = batch([{"command": "play.start"}, {"command": "play.step", "params": {"frames": 2}}, {"command": "play.stop"}])
        if "ran 100000 instructions without returning" not in played.stdout:
            fail(f"the CLI did not stop the script: {played.stdout}{played.stderr}")

        finished = run(str(runtime), "--project", str(project), "--headless", "--frames", "3", "--fail-on-script-errors")
        if finished.returncode != 0:
            fail(f"the runtime stopped the script (exit {finished.returncode}): {finished.stdout}{finished.stderr}")

        # Its smoke test plays the exported game, and fails on script errors.
        exported = batch([{"command": "project.export", "params": {"output": str(Path(temp) / "Exported"), "smokeTestFrames": 3}}])
        if exported.returncode != 0:
            fail(f"the exported game stopped the script: {exported.stdout}{exported.stderr}")

        script.write_text("local Long = {}\nfunction Long:OnUpdate(dt)\n\twhile true do end\nend\nreturn Long\n", encoding="utf-8")
        endless = batch([{"command": "play.start"}, {"command": "play.step", "params": {"frames": 2}}, {"command": "play.stop"}])
        if "ran 100000 instructions without returning" not in endless.stdout:
            fail(f"the CLI did not stop the endless script: {endless.stdout}{endless.stderr}")
    print("OK")


if __name__ == "__main__":
    main()
