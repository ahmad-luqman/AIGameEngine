"""Exports the feature test project as a folder and as an archive and plays both (ctest ArchiveExport).

The feature project uses every component and asset kind (scenes, prefabs, materials with UUID texture references,
animation graphs, glTF, KTX2, fonts, audio, particles, required Lua modules). Both exports run headless for the same
frames; their logs must match line for line apart from the archive's mount message, so any loader that bypassed the
archive (and logged a failed load while the game still exited) shows up as a difference.

Usage: ArchiveExport.py <basalt CLI> <feature test project>
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

FRAMES = 200


def fail(message):
    print(f"FAIL: {message}")
    sys.exit(1)


def main():
    cli, source = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="BasaltArchive-") as temp:
        root = Path(temp)
        env = dict(os.environ, BASALT_USER_DATA_DIR=str(root / "UserData"))
        if sys.platform == "darwin":
            # Export bundles the Vulkan loader and MoltenVK; headless runs never load them, so stand-ins do.
            sdk = root / "VulkanSDK" / "lib"
            sdk.mkdir(parents=True)
            for name in ("libvulkan.1.dylib", "libMoltenVK.dylib"):
                (sdk / name).write_text("stand-in")
            env["VULKAN_SDK"] = str(sdk.parent)
        project = root / "FeatureTest"
        shutil.copytree(source, project, ignore=shutil.ignore_patterns("Cache", "*.batch.json"))

        logs = {}
        for form, archive in (("folder", False), ("archive", True)):
            output = root / form
            run = subprocess.run([str(cli), "--project", str(project), "--no-save", "project.export", json.dumps({"output": str(output), "archive": archive})], capture_output=True, text=True, timeout=300, env=env)
            start = run.stdout.find("{")
            response = json.loads(run.stdout[start:]) if start >= 0 else {}
            if not response.get("ok"):
                fail(f"{form} export: {run.stdout}{run.stderr}")
            if archive != (output / "Assets.bpak").is_file() or archive == (output / "Assets").is_dir():
                fail(f"{form} export has the wrong layout: {sorted(p.name for p in output.iterdir())}")
            game = subprocess.run([response["result"]["executable"], "--headless", "--frames", str(FRAMES)], capture_output=True, text=True, errors="replace", timeout=300, env=env, cwd=output)
            if game.returncode != 0:
                fail(f"the {form} export exited with {game.returncode}: {game.stdout}{game.stderr}")
            lines = []
            for line in (game.stdout + game.stderr).splitlines():
                line = re.sub(r"^\[\d\d:\d\d:\d\d\] ", "", line).replace(str(output), "<export>")
                if "reading assets from" in line:
                    continue
                lines.append(line)
            logs[form] = sorted(lines)

        folder, archive = set(logs["folder"]), set(logs["archive"])
        only_archive = sorted(archive - folder)
        only_folder = sorted(folder - archive)
        if only_archive or only_folder:
            fail("the exports' logs differ:\n  only archived:\n    " + "\n    ".join(only_archive) + "\n  only folder:\n    " + "\n    ".join(only_folder))
        print(f"ArchiveExport: both exports logged the same {len(logs['folder'])} lines over {FRAMES} frames")


if __name__ == "__main__":
    main()
