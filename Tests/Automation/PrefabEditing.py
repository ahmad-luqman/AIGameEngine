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
    require(call("prefab.create", {"entity": root, "path": path})["linked"], "prefab.create did not link its tree")
    call("entity.destroy", {"entity": root})
    instance = call("prefab.instantiate", {"path": path, "position": [3, 0, 0]})["id"]
    call("component.set", {"entity": instance, "component": "PointLight", "data": {"Intensity": 7}})
    overrides = call("prefab.overrides", {"entity": instance})
    # The prefab file has its own entity IDs; members maps them to the instance's.
    source_root = int(next(key for key, value in overrides["members"].items() if value == instance))
    field = f"/{source_root}/Components/PointLight/Intensity"
    require(field in overrides["overrides"], "Instance did not record a field override")
    if output:
        call("editor.select", {"entity": instance})
        call("editor.panel", {"name": "Inspector", "focus": True})
        call("editor.window_screenshot", {"path": str(Path(output, "prefab-instance.png"))})
    call("prefab.open", {"path": path})
    call("component.set", {"entity": source_root, "component": "PointLight", "data": {"Radius": 20}})
    if output:
        call("editor.select", {"entity": source_root})
        call("editor.window_screenshot", {"path": str(Path(output, "prefab-editing.png"))})
    saved = call("prefab.save")
    require(saved["instances"] == 1 and not saved["warnings"], f"Saving did not merge the instance: {saved}")
    call("prefab.close")
    call("assert.component", {"entity": instance, "component": "PointLight", "fields": {"Radius": 20, "Intensity": 7}})
    if output:
        # The scene's undo history survives prefab editing; the merge into it is one more step.
        require(call("editor.history")["undo"] == "Merge Prefab Changes", "Prefab merge is not an undo step")
        call("editor.undo")
        call("assert.component", {"entity": instance, "component": "PointLight", "fields": {"Intensity": 7}})
        require(call("component.get", {"entity": instance, "component": "PointLight"})["Radius"] != 20, "Undo kept the merge")
        # The steps from before prefab.open are still there.
        history = call("editor.history")
        require(history["undo"] == "Set PointLight Intensity", f"Undo history before prefab editing was lost: {history}")
        call("editor.undo")
        call("assert.component", {"entity": instance, "component": "PointLight", "fields": {"Intensity": 2}})
        call("editor.redo")
        call("editor.redo")
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
    refreshed = call("prefab.refresh", {"entity": instance})
    require(refreshed["tracked"] and field not in refreshed["overrides"], f"Refresh changed the applied instance: {refreshed}")
    call("scene.save", {"path": "Assets/Scenes/PrefabAuthoring.bscene"})
    call("scene.open", {"path": "Assets/Scenes/PrefabAuthoring.bscene"})
    require(field not in call("prefab.overrides", {"entity": instance})["overrides"], "Save/load created a false override")
    call("prefab.unpack", {"entity": instance})
    call("assert.component", {"entity": instance, "component": "Prefab", "exists": False})
    call("prefab.open", {"path": path})
    call("entity.rename", {"entity": source_root, "name": "Discarded"})
    call("prefab.close", {"discard": True})
    call("assert.entity_exists", {"entity": "Discarded", "count": 0})


def check_nesting(call, require):
    """An inner prefab's edit reaches the instances of the prefabs nesting and inheriting it, across a save and reload."""
    call("scene.new", {"empty": True})
    inner, outer, variant = (f"Assets/Prefabs/{name}.bprefab" for name in ("Inner", "Outer", "InnerVariant"))
    root = call("entity.create", {"name": "Inner", "components": {"PointLight": {"Radius": 2}}})["id"]
    call("prefab.create", {"entity": root, "path": inner})
    holder = call("entity.create", {"name": "Holder"})["id"]
    call("prefab.instantiate", {"path": inner, "parent": holder})
    saved = call("prefab.create", {"entity": holder, "path": outer})
    require(saved["linked"] and not saved["warnings"], f"prefab.create did not keep the nested instance: {saved}")
    call("prefab.variant", {"base": inner, "path": variant})
    info = call("prefab.info", {"path": variant})
    require(info["base"] == inner and info["entities"] == 1, f"prefab.info of a variant: {info}")
    nesting = call("prefab.instantiate", {"path": outer})["id"]
    inheriting = call("prefab.instantiate", {"path": variant})["id"]
    members = call("prefab.overrides", {"entity": nesting})["members"]
    nested = next(value for value in members.values() if value != nesting)
    call("component.set", {"entity": root, "component": "PointLight", "data": {"Radius": 11}})
    call("prefab.apply", {"entity": root})
    for entity in (nested, inheriting):
        call("assert.component", {"entity": entity, "component": "PointLight", "fields": {"Radius": 11}})
    require(not call("prefab.overrides", {"entity": nesting})["overrides"], "An inner edit read as an outer override")
    call("scene.save", {"path": "Assets/Scenes/PrefabNesting.bscene"})
    # The prefab changes behind the scene's back, as a pull would: opening merges it through the outer prefab.
    call("scene.new", {"empty": True})
    call("prefab.open", {"path": inner})
    call("component.set", {"entity": "Inner", "component": "PointLight", "data": {"Radius": 12}})
    call("prefab.save")
    call("prefab.close")
    opened = call("scene.open", {"path": "Assets/Scenes/PrefabNesting.bscene"})
    require(opened.get("prefabInstances", 0) >= 3, f"Opening did not merge the outdated instances: {opened}")
    call("assert.component", {"entity": nested, "component": "PointLight", "fields": {"Radius": 12}})
    require(call("prefab.overrides", {"entity": nested})["enclosing"] == nesting, "The nested root lost its instance")


def main():
    cli = str(Path(sys.argv[1]).resolve())
    source = Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="BasaltPrefabs-") as temp:
        project = Path(temp, "Project")
        # Saved/ holds the FeatureTest batch's replay, which that test may be writing right now (ctest -j).
        shutil.copytree(source, project, ignore=shutil.ignore_patterns("Saved"))
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
                check_nesting(call, require)
                process.stdin.close()
                require(process.wait(timeout=10) == 0, "CLI failed on exit")
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)
        # A one-shot run that leaves a prefab open with unsaved edits fails instead of dropping them silently.
        def batch(name, commands, *options):
            file = Path(temp, name)
            file.write_text(json.dumps(commands))
            return subprocess.run([cli, "--project", str(project), *options, "batch", str(file)], capture_output=True, text=True, timeout=60)
        saved = batch("save-scene.json", [{"command": "scene.new", "params": {"empty": True}}, {"command": "scene.save", "params": {"path": "Assets/Scenes/OpenPrefab.bscene"}}])
        require(saved.returncode == 0, f"Could not prepare the scene: {saved.stdout} {saved.stderr}")
        result = batch("open-prefab.json", [{"command": "entity.create", "params": {"name": "Unsaved"}}, {"command": "prefab.open", "params": {"path": "Assets/Prefabs/Authoring.bprefab"}}], "--scene", "Assets/Scenes/OpenPrefab.bscene")
        require(result.returncode != 0 and "is still open" in result.stderr, f"Exit with an open prefab did not fail: {result.returncode} {result.stderr}")
        # Nothing unsaved: leaving the prefab open is harmless.
        clean = batch("open-clean.json", [{"command": "prefab.open", "params": {"path": "Assets/Prefabs/Authoring.bprefab"}}], "--scene", "Assets/Scenes/OpenPrefab.bscene")
        require(clean.returncode == 0, f"Exit with a clean open prefab failed: {clean.returncode} {clean.stderr}")
    print("Prefab feature integration passed")


if __name__ == "__main__":
    main()
