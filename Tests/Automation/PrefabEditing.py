"""Prefab authoring through the CLI and editor, against an isolated copy of the feature project."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def check_prefabs(call, require, output=None):
    call("scene.new", {"empty": True})
    root = call("entity.create", {"name": "PrefabSource", "components": {"PointLight": {"Intensity": 2}, "Mesh": {"Mesh": "builtin://Cube"}}})["id"]
    call("entity.create", {"name": "PrefabChild", "parent": root})
    path = "Assets/Prefabs/Authoring.bprefab"
    call("prefab.create", {"entity": root, "path": path})
    call("entity.destroy", {"entity": root})
    instance = call("prefab.instantiate", {"path": path, "position": [3, 0, 0]})["id"]
    call("component.set", {"entity": instance, "component": "PointLight", "data": {"Intensity": 7}})
    field = f"/{root}/Components/PointLight/Intensity"
    overrides = call("prefab.overrides", {"entity": instance})
    require(field in overrides["overrides"], "Instance did not record a field override")
    require(str(root) in overrides["members"], "Instance did not map its source entities")
    if output:
        call("editor.select", {"entity": instance})
        call("editor.panel", {"name": "Inspector", "focus": True})
        call("editor.window_screenshot", {"path": str(Path(output, "prefab-instance.png"))})
    call("prefab.open", {"path": path})
    call("component.set", {"entity": root, "component": "PointLight", "data": {"Radius": 20}})
    if output:
        call("editor.select", {"entity": root})
        call("editor.window_screenshot", {"path": str(Path(output, "prefab-editing.png"))})
    saved = call("prefab.save")
    require(saved["instances"] == 1 and not saved["warnings"], f"Saving did not merge the instance: {saved}")
    call("prefab.close")
    call("assert.component", {"entity": instance, "component": "PointLight", "fields": {"Radius": 20, "Intensity": 7}})
    call("prefab.revert", {"entity": instance, "field": field})
    call("assert.component", {"entity": instance, "component": "PointLight", "fields": {"Intensity": 2}})
    if output:
        call("editor.undo")
        call("assert.component", {"entity": instance, "component": "PointLight", "fields": {"Intensity": 7}})
        call("editor.redo")
    call("component.set", {"entity": instance, "component": "PointLight", "data": {"Intensity": 9}})
    call("prefab.apply", {"entity": instance})
    require(field not in call("prefab.overrides", {"entity": instance})["overrides"], "Applied field remained overridden")
    other = call("prefab.instantiate", {"path": path})["id"]
    call("assert.component", {"entity": other, "component": "PointLight", "fields": {"Intensity": 9}})
    call("prefab.refresh", {"entity": instance})
    call("scene.save", {"path": "Assets/Scenes/PrefabAuthoring.bscene"})
    call("scene.open", {"path": "Assets/Scenes/PrefabAuthoring.bscene"})
    require(field not in call("prefab.overrides", {"entity": instance})["overrides"], "Save/load created a false override")
    call("prefab.unpack", {"entity": instance})
    call("assert.component", {"entity": instance, "component": "Prefab", "exists": False})
    call("prefab.open", {"path": path})
    call("entity.rename", {"entity": root, "name": "Discarded"})
    call("prefab.close", {"discard": True})
    call("assert.entity_exists", {"entity": "Discarded", "count": 0})


def main():
    cli = str(Path(sys.argv[1]).resolve())
    source = Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="BasaltPrefabs-") as temp:
        project = Path(temp, "Project")
        shutil.copytree(source, project)
        with Path(temp, "cli.log").open("w+") as log:
            process = subprocess.Popen([cli, "--project", str(project), "--no-save", "serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True)
            def require(value, message):
                if not value:
                    raise RuntimeError(message)
            def call(command, params=None):
                process.stdin.write(json.dumps({"command": command, "params": params or {}}) + "\n")
                process.stdin.flush()
                response = json.loads(process.stdout.readline())
                require(response.get("ok"), f"{command}: {response}")
                return response["result"]
            try:
                check_prefabs(call, require)
                process.stdin.close()
                require(process.wait(timeout=10) == 0, "CLI failed on exit")
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)
    print("Prefab feature integration passed")


if __name__ == "__main__":
    main()
