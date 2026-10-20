"""Exercise identity commands and UUID persistence against an isolated copy of the feature project."""
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

def require(condition, message):
    if not condition:
        raise AssertionError(message)


cli = pathlib.Path(sys.argv[1]).resolve()
source = pathlib.Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory(prefix="BasaltAssetPipeline-") as temp:
    project = pathlib.Path(temp) / "Project"
    shutil.copytree(source, project)
    original = "Assets/Textures/Checker.png"
    target = "Assets/Textures/Renamed.png"
    commands = [
        {"command": "scene.open", "params": {"path": "Assets/Scenes/FeatureTest.bscene"}},
        {"command": "asset.id", "params": {"path": original}},
        {"command": "asset.move", "params": {"from": original, "to": target}},
        {"command": "scene.save"},
    ]
    batch = pathlib.Path(temp) / "Move.json"
    batch.write_text(json.dumps(commands))
    result = subprocess.run([str(cli), "--project", str(project), "--no-save", "batch", str(batch)], capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    meta = json.loads((project / (target + ".meta")).read_text())
    require(not (project / original).exists(), "source was not moved")
    require(not (project / (original + ".meta")).exists(), "source metadata was not moved")
    resolved = subprocess.run([str(cli), "--project", str(project), "asset.resolve", json.dumps({"uuid": meta["UUID"]})], capture_output=True, text=True, timeout=30)
    if resolved.returncode:
        raise AssertionError(resolved.stdout + resolved.stderr)
    require(json.loads(resolved.stdout)["result"]["path"] == target, "UUID did not resolve to the moved file")
    saved = json.loads((project / "Assets/Scenes/FeatureTest.bscene").read_text())
    materials = [e["Components"]["Material"]["AlbedoMap"] for e in saved["Entities"] if "Material" in e.get("Components", {}) and "AlbedoMap" in e["Components"]["Material"]]
    require(any(isinstance(value, dict) and value.get("UUID") == meta["UUID"] for value in materials), "scene did not persist the moved texture UUID")
    # Load while still renamed, then prove that moving back invalidates the path hint rather than the UUID.
    commands = [
        {"command": "scene.open", "params": {"path": "Assets/Scenes/FeatureTest.bscene"}},
        {"command": "asset.rename", "params": {"from": target, "to": original}},
    ] + json.loads((project / "FeatureTest.batch.json").read_text())
    batch.write_text(json.dumps(commands))
    result = subprocess.run([str(cli), "--project", str(project), "--no-save", "batch", str(batch)], capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    require(json.loads((project / (original + ".meta")).read_text())["UUID"] == meta["UUID"], "rename changed the asset UUID")
    delete_batch = [
        {"command": "asset.write", "params": {"path": "Assets/DeleteProbe.lua", "content": "return {}"}},
        {"command": "asset.id", "params": {"path": "Assets/DeleteProbe.lua"}},
        {"command": "asset.delete", "params": {"path": "Assets/DeleteProbe.lua"}},
    ]
    batch.write_text(json.dumps(delete_batch))
    result = subprocess.run([str(cli), "--project", str(project), "--no-save", "batch", str(batch)], capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    require(not (project / "Assets/DeleteProbe.lua.meta").exists(), "deletion left a reusable identity")
    missing_batch = [
        {"command": "asset.delete", "params": {"path": original}},
        {"command": "scene.open", "params": {"path": "Assets/Scenes/FeatureTest.bscene"}},
        {"command": "scene.save"},
        {"command": "asset.resolve", "params": {"uuid": meta["UUID"]}, "expectError": "unresolved"},
    ]
    batch.write_text(json.dumps(missing_batch))
    result = subprocess.run([str(cli), "--project", str(project), "--no-save", "batch", str(batch)], capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    saved = json.loads((project / "Assets/Scenes/FeatureTest.bscene").read_text())
    require(any(e.get("Components", {}).get("Material", {}).get("AlbedoMap", {}).get("UUID") == meta["UUID"] for e in saved["Entities"] if isinstance(e.get("Components", {}).get("Material", {}).get("AlbedoMap"), dict)), "saving an unresolved reference lost its UUID")
    print("Asset IDs, move/rename, persisted references and feature playback passed")
