"""Passes command-line arguments outside every single-byte code page to the basalt CLI and the runtime (ctest
UnicodeArguments).

Windows hands a program's narrow argv over in the ANSI code page (1252 on CI), which has no CJK characters: the CLI and
the runtime read the wide command line instead (Platform::GetProcessArguments). Python starts them with the arguments
as UTF-16, so this checks what a user's shell would pass: a project folder, JSON parameters (the characters themselves,
not \\u escapes), a batch file and a scene, all named in CJK characters. With the runtime, the project is then exported
(as a folder and as Assets.bpak) into a CJK folder and played from there without --project, as a player who installed
it under their own (Japanese, say) user folder would: the start scene's script requires a CJK module.

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


def run(arguments, env, cwd=None):
    """The process's stdout (JSON responses) and stderr (logs), after checking it succeeded."""
    # Decoded as UTF-8, not Python's locale encoding (cp1252 on Windows CI): the CLI and the runtime write UTF-8 bytes.
    result = subprocess.run([str(argument) for argument in arguments], capture_output=True, encoding="utf-8", errors="replace", timeout=120, env=env, cwd=cwd)
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
            {"command": "asset.write", "params": {"path": "Assets/Scripts/道具.lua", "content": "return { Value = 42 }"}},
            {"command": "asset.write", "params": {"path": "Assets/Scripts/主役.lua", "content": "local Tool = require('Assets.Scripts.道具')\nassert(Tool.Value == 42)\nreturn {}"}},
            {"command": "scene.new", "params": {"name": "場面"}},
            {"command": "entity.create", "params": {"name": "主役", "components": {"Script": {"Script": "Assets/Scripts/主役.lua"}}}},
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
            log = run([runtime, "--project", project, "--scene", "Assets/Scenes/場面.bscene", "--headless", "--frames", "3", "--fail-on-script-errors"], env)[1]
            if "Runtime: playing 'Assets/Scenes/場面.bscene'" not in log:
                fail(f"the runtime did not play the scene:\n{log}")
            if sys.platform == "darwin":
                # Export bundles the Vulkan loader and MoltenVK; headless runs never load them, so stand-ins do.
                sdk = root / "VulkanSDK" / "lib"
                sdk.mkdir(parents=True)
                for name in ("libvulkan.1.dylib", "libMoltenVK.dylib"):
                    (sdk / name).write_text("stand-in")
                env["VULKAN_SDK"] = str(sdk.parent)
            for form, archive in (("フォルダ", False), ("書庫", True)):
                output = root / "出力 遊戯" / form
                exported = response(run([cli, "--project", project, "--no-save", "project.export", json.dumps({"output": str(output), "archive": archive}, ensure_ascii=False)], env)[0])
                if not exported.get("ok") or archive != (output / "Assets.bpak").is_file():
                    fail(f"exporting to {output}: {exported}")
                # Found through the executable's own (CJK) folder: no --project.
                log = run([exported["result"]["executable"], "--headless", "--frames", "3", "--fail-on-script-errors"], env, cwd=output)[1]
                if "Runtime: playing 'Assets/Scenes/場面.bscene'" not in log:
                    fail(f"the {form} export did not play its start scene:\n{log}")
    print("PASS")


if __name__ == "__main__":
    main()
