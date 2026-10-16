#!/usr/bin/env python3
"""Validate, export and smoke-test a project through the basalt CLI, as a release pipeline would (no GPU)."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

SCENE = {
    "Format": "BasaltScene",
    "Version": 1,
    "Name": "Main",
    "Entities": [
        {"ID": 1, "Name": "Camera", "Components": {
            "Transform": {"Translation": [0, 1, 5]}, "Camera": {"FOV": 50, "Near": 0.1, "Far": 100}}},
        {"ID": 2, "Name": "Crate", "Components": {
            "Mesh": {"Mesh": "Assets/Models/Crate.glb"},
            "Script": {"Script": "Assets/Scripts/Spin.lua"}}},
    ],
}
# Saves on start, so the test can check where the smoke run's save data went.
GOOD_SCRIPT = "local Spin = {}\nfunction Spin:OnCreate()\n\tStorage.Set('Launched', true)\n\tassert(Storage.Save())\nend\nfunction Spin:OnUpdate(dt)\n\tself.Entity.Rotation = Quat.AngleAxis(dt, Vec3(0, 1, 0)) * self.Entity.Rotation\nend\nreturn Spin\n"
BROKEN_SCRIPT = "local Spin = {}\nfunction Spin:OnUpdate(dt)\n\terror('spin failed')\nend\nreturn Spin\n"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    cli, model = map(lambda value: str(Path(value).resolve()), sys.argv[1:3])
    # Spaces in every path: the smoke test must quote the exported executable and run it from its own folder.
    with tempfile.TemporaryDirectory(prefix="Basalt Export ") as temp:
        env = dict(os.environ, BASALT_USER_DATA_DIR=str(Path(temp, "User Data")))
        if sys.platform == "darwin":
            # Export bundles the Vulkan loader and MoltenVK; headless runs never load them, so stand-ins do.
            sdk = Path(temp, "Vulkan SDK", "lib")
            sdk.mkdir(parents=True)
            for name in ("libvulkan.1.dylib", "libMoltenVK.dylib"):
                (sdk / name).write_text("stand-in")
            env["VULKAN_SDK"] = str(sdk.parent)
        project = Path(temp, "Smoke Game")

        def run(command, params=None, project_dir=project):
            arguments = [cli] + (["--project", str(project_dir)] if project_dir else []) + [command, json.dumps(params or {})]
            completed = subprocess.run(arguments, capture_output=True, text=True, timeout=300, env=env)
            start = completed.stdout.find("{")
            require(start >= 0, f"{command}: no JSON in output: {completed.stdout}{completed.stderr}")
            response = json.loads(completed.stdout[start:])
            require((completed.returncode == 0) == response.get("ok"), f"{command}: exit code {completed.returncode} disagrees with {response}")
            return response

        def ok(command, params=None, project_dir=project):
            response = run(command, params, project_dir)
            require(response.get("ok"), f"{command}: {response}")
            return response["result"]

        def fails(command, params, expected):
            response = run(command, params)
            require(not response.get("ok") and expected in response.get("error", ""), f"{command}: expected an error containing {expected!r}, got {response}")
            return response["error"]

        ok("project.create", {"path": str(project), "name": "Smoke Game"}, None)
        shutil.copy(model, project / "Assets" / "Models" / "Crate.glb")
        (project / "Assets" / "Scripts" / "Spin.lua").write_text(GOOD_SCRIPT)
        (project / "Assets" / "Scenes" / "Main.bscene").write_text(json.dumps(SCENE))
        ok("project.set", {"startScene": "Assets/Scenes/Main.bscene"})

        report = ok("project.validate")
        require(report["valid"] and report["errors"] == 0 and report["warnings"] == 0, f"clean project reported {report}")

        output = Path(temp, "Exported Build")
        exported = ok("project.export", {"output": str(output), "smokeTestFrames": 30})
        executable = Path(exported["executable"])
        require(executable.parent == output and executable.stem == "Smoke_Game", f"unexpected executable {executable}")
        smoke = exported["smokeTest"]
        require(smoke["passed"] and smoke["outcome"] == "exited" and smoke["exitCode"] == 0, f"smoke test: {smoke}")
        require("Runtime: playing 'Assets/Scenes/Main.bscene'" in smoke["log"], f"smoke log: {smoke['log']}")
        require(exported["validation"]["valid"], "export did not report its validation")
        # The game's save data went to a temporary directory, not the user's.
        require(not Path(temp, "User Data", "Smoke Game").exists(), "the smoke test wrote save data to the user directory")

        rerun = ok("project.smoke_test", {"executable": str(executable), "frames": 5, "assertPassed": True})
        require(rerun["passed"], f"standalone smoke test: {rerun}")

        # A script that fails at runtime still compiles, so validation passes and only the smoke test catches it.
        (project / "Assets" / "Scripts" / "Spin.lua").write_text(BROKEN_SCRIPT)
        require(ok("project.validate")["valid"], "a runtime script error should not fail validation")
        error = fails("project.export", {"output": str(output), "smokeTestFrames": 10}, "exited with code 7: scripts reported errors")
        require("spin failed" in error, f"the failure does not show the script error: {error}")
        failed = ok("project.smoke_test", {"executable": str(executable), "frames": 10})
        require(not failed["passed"] and failed["exitCode"] == 7 and "spin failed" in failed["log"], f"smoke test of a broken build: {failed}")
        fails("project.smoke_test", {"executable": str(executable), "frames": 10, "assertPassed": True}, "smoke test failed")

        # A missing asset is caught before anything is exported.
        (project / "Assets" / "Scripts" / "Spin.lua").write_text(GOOD_SCRIPT)
        (project / "Assets" / "Models" / "Crate.glb").unlink()
        report = ok("project.validate")
        require(not report["valid"] and any("Assets/Models/Crate.glb" in issue["message"] for issue in report["issues"]), f"missing model: {report}")
        before = executable.stat().st_mtime_ns
        fails("project.export", {"output": str(output), "smokeTestFrames": 10}, "not exported: project validation found 1 error")
        require(executable.stat().st_mtime_ns == before, "a refused export modified the previous build")
    print("ExportSmoke: OK")


if __name__ == "__main__":
    main()
