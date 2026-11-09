"""The hosts that edit a game stop a script at ScriptInstructionLimit; the runtime, which plays one, never does (ctest
ScriptInstructionLimit). One script, over the limit but finite, fails in the CLI and finishes in BrecciaRuntime, which
every exported game is. An endless one then fails in the CLI, which must answer rather than hang: the timeouts are the
check.

Usage: ScriptInstructionLimit.py <breccia CLI> <BrecciaRuntime>
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

TIMEOUT = 120
STOPPED = "ran 100000 instructions without returning"


def require(condition, message):
    if not condition:
        print(f"FAIL: {message}")
        sys.exit(1)


def update_script(body):
    return f"local Long = {{}}\nfunction Long:OnUpdate(dt)\n\t{body}\nend\nreturn Long\n"


def main():
    cli, runtime = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory() as temp:
        project = Path(temp) / "Limited"

        def run(*arguments):
            return subprocess.run(arguments, capture_output=True, text=True, timeout=TIMEOUT, env={**os.environ, "BRECCIA_USER_DATA_DIR": str(Path(temp) / "User")})

        def batch(requests):
            path = Path(temp) / "batch.json"
            path.write_text(json.dumps(requests), encoding="utf-8")
            return run(str(cli), "--project", str(project), "batch", str(path))

        def play():
            return batch([{"command": "play.start"}, {"command": "play.step", "params": {"frames": 2}}, {"command": "play.stop"}])

        created = run(str(cli), "project.create", json.dumps({"path": str(project), "name": "Limited"}))
        require(created.returncode == 0, f"project.create: {created.stdout}{created.stderr}")
        script = project / "Assets" / "Scripts" / "Long.lua"
        script.write_text(update_script("for i = 1, 2000000 do end"), encoding="utf-8")
        built = batch([
            {"command": "scene.new", "params": {"name": "Main"}},
            {"command": "entity.create", "params": {"name": "Busy", "components": {"Script": {"Script": "Assets/Scripts/Long.lua"}}}},
            {"command": "scene.save", "params": {"path": "Assets/Scenes/Main.bscene"}},
            {"command": "project.set", "params": {"startScene": "Assets/Scenes/Main.bscene", "scriptInstructionLimit": 100000}},
        ])
        require(built.returncode == 0, f"building the scene: {built.stdout}{built.stderr}")

        played = play()
        require(STOPPED in played.stdout, f"the CLI did not stop the script: {played.stdout}{played.stderr}")
        finished = run(str(runtime), "--project", str(project), "--headless", "--frames", "3", "--fail-on-script-errors")
        require(finished.returncode == 0, f"the runtime stopped the script (exit {finished.returncode}): {finished.stdout}{finished.stderr}")

        script.write_text(update_script("while true do end"), encoding="utf-8")
        endless = play()
        require(STOPPED in endless.stdout, f"the CLI did not stop the endless script: {endless.stdout}{endless.stderr}")
    print("OK")


if __name__ == "__main__":
    main()
