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
    compression = {"path": "Assets/Textures/Checker.png", "output": "Assets/Textures/Checker.ktx2", "srgb": True}
    def command(name, params):
        result = subprocess.run([str(cli), "--project", str(project), name, json.dumps(params)], capture_output=True, text=True, timeout=60)
        require(result.returncode == 0, result.stdout + result.stderr)
        return json.loads(result.stdout)["result"]
    def batch_results(commands):
        path = pathlib.Path(temp) / "Steps.json"
        path.write_text(json.dumps(commands))
        result = subprocess.run([str(cli), "--project", str(project), "--no-save", "batch", str(path)], capture_output=True, text=True, timeout=120)
        require(result.returncode == 0, result.stdout + result.stderr)
        return [step["result"] for step in json.loads(result.stdout)]
    source_bytes = (project / compression["path"]).read_bytes()
    first = command("asset.compress_texture", compression)
    require(first["cacheHit"] is False and first["overwrote"] is False and "warnings" not in first, "first compression: " + json.dumps(first))
    repeat = command("asset.compress_texture", compression)
    require(repeat["cacheHit"] is True and repeat["overwrote"] is True, "repeat compression: " + json.dumps(repeat))
    # Recompressing a different source over a texture the session holds reloads it, so the new pixels are served.
    held = batch_results([
        {"command": "asset.preload", "params": {"paths": [compression["output"]], "wait": True}},
        {"command": "memory.stats"},
        {"command": "asset.compress_texture", "params": dict(compression, path="Assets/Textures/UIPanel.png")},
        {"command": "asset.preload", "params": {"paths": [compression["output"]], "wait": True}},
        {"command": "memory.stats"},
    ])
    require(held[2]["reloaded"] == [compression["output"]], "held texture was not reloaded: " + json.dumps(held[2]))
    require(held[3][0]["status"] == "ready" and held[4]["assets"]["bytes"] < held[1]["assets"]["bytes"], "recompressed 32x32 texture was not served: " + json.dumps(held))
    command("asset.compress_texture", compression)
    # A new output next to an orphaned .meta adopts its UUID, and says so.
    command("asset.id", {"path": compression["output"]})
    orphan_meta = (project / (compression["output"] + ".meta")).read_text()
    (project / compression["output"]).unlink()
    adopted = command("asset.compress_texture", compression)
    require(adopted["overwrote"] is False and any("orphan metadata" in w for w in adopted.get("warnings", [])), "orphan adoption not reported: " + json.dumps(adopted))
    require((project / (compression["output"] + ".meta")).read_text() == orphan_meta, "adoption changed the output UUID")
    require((project / compression["path"]).read_bytes() == source_bytes, "compression changed source")
    require(command("asset.preload", {"paths": [compression["output"]], "wait": True})[0]["status"] == "ready", "KTX2 did not load")
    for invalid in [dict(compression, output="../escape.ktx2"), dict(compression, output="Assets/wrong.png"), dict(compression, srgb="true")]:
        result = subprocess.run([str(cli), "--project", str(project), "asset.compress_texture", json.dumps(invalid)], capture_output=True, text=True, timeout=30)
        require(result.returncode != 0, "invalid compression request succeeded")
    if sys.platform != "win32":
        alias = project / "Assets/Textures/Alias.png"
        alias.symlink_to(project / compression["path"])
        result = subprocess.run([str(cli), "--project", str(project), "asset.compress_texture", json.dumps(dict(compression, path="Assets/Textures/Alias.png"))], capture_output=True, text=True, timeout=30)
        require(result.returncode != 0, "compression accepted a symlink source")
        alias.unlink()
    if sys.platform != "win32":
        def reject(params, reason):
            result = subprocess.run([str(cli), "--project", str(project), "asset.compress_texture", json.dumps(params)], capture_output=True, text=True, timeout=30)
            require(result.returncode != 0 and reason in result.stdout + result.stderr, "missing diagnostic: " + reason)
        reject(dict(compression, output="Cache/out.ktx2"), "under Assets")
        outside = pathlib.Path(temp) / "Outside"
        outside.mkdir()
        (project / "Assets/AliasDirectory").symlink_to(outside, target_is_directory=True)
        reject(dict(compression, output="Assets/AliasDirectory/out.ktx2"), "symlinks")
        (project / "Assets/AliasDirectory").unlink()
        leaf = project / "Assets/Alias.ktx2"
        leaf.symlink_to(outside / "untouched.ktx2")
        reject(dict(compression, output="Assets/Alias.ktx2"), "symlinks")
        leaf.unlink()
        # The cache is optional: a linked cache folder is skipped with a warning instead of written through.
        shutil.rmtree(project / "Cache/Textures")
        (project / "Cache/Textures").symlink_to(outside, target_is_directory=True)
        uncached = command("asset.compress_texture", compression)
        require(uncached["cacheHit"] is False and any("without the cache" in w and "symlink" in w for w in uncached.get("warnings", [])), "linked cache not reported: " + json.dumps(uncached))
        (project / "Cache/Textures").unlink()
        require(not list(outside.iterdir()), "invalid path request wrote outside project")
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
    extended = next(e["Components"]["Material"] for e in saved["Entities"] if e["Name"] == "MaterialExtensionData")
    for field in ("ClearcoatMap", "ClearcoatRoughnessMap", "ClearcoatNormalMap", "TransmissionMap"):
        require(extended[field]["UUID"] == meta["UUID"], field + " did not encode moved texture identity")
    # Material files reference textures too: the move rewrote the feature material's path as the texture's UUID.
    checker_material = json.loads((project / "Assets/Materials/Checker.bmaterial").read_text())
    # Like scenes, the migration records the identity with the path it had; the UUID is what resolves.
    require(checker_material["AlbedoMap"].get("UUID") == meta["UUID"], "material file did not encode moved texture identity: " + json.dumps(checker_material))
    require(checker_material["NormalMap"] == "Assets/Textures/FlatNormal.png", "material references to other files changed")
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
    # A model's materials extract losslessly to files whose embedded images keep the model's identity; renaming a
    # material file migrates the scene references to it.
    extracted = command("material.extract", {"model": "Assets/Models/TexturedCube.glb"})
    require([m["path"] for m in extracted["materials"]] == ["Assets/Models/Materials/Checker.bmaterial"], "extract: " + json.dumps(extracted))
    extracted_file = json.loads((project / "Assets/Models/Materials/Checker.bmaterial").read_text())
    model_meta = json.loads((project / "Assets/Models/TexturedCube.glb.meta").read_text())
    require(extracted_file["AlbedoMap"] == {"UUID": model_meta["UUID"], "Path": "Assets/Models/TexturedCube.glb#image/0"}, "extracted embedded image reference: " + json.dumps(extracted_file))
    require(extracted_file["Roughness"] == 0.6 and extracted_file["Metallic"] == 0, "extracted factors: " + json.dumps(extracted_file))
    renamed = batch_results([
        {"command": "scene.open", "params": {"path": "Assets/Scenes/FeatureTest.bscene"}},
        {"command": "asset.rename", "params": {"from": "Assets/Materials/Checker.bmaterial", "to": "Assets/Materials/Tiles.bmaterial"}},
        {"command": "scene.save"},
        {"command": "asset.preload", "params": {"paths": ["Assets/Materials/Tiles.bmaterial", "Assets/Models/Materials/Checker.bmaterial"], "wait": True}},
        {"command": "project.validate", "params": {"assertValid": True}},
    ])
    require(all(entry["status"] == "ready" for entry in renamed[3]), "moved or extracted materials did not load: " + json.dumps(renamed[3]))
    saved = json.loads((project / "Assets/Scenes/FeatureTest.bscene").read_text())
    material_meta = json.loads((project / "Assets/Materials/Tiles.bmaterial.meta").read_text())
    pillars = [e["Components"]["Material"]["Material"] for e in saved["Entities"] if e["Name"] == "Pillar"]
    require({"UUID": material_meta["UUID"], "Path": "Assets/Materials/Tiles.bmaterial"} in pillars, "scene did not persist the renamed material: " + json.dumps(pillars))
    require({"UUID": model_meta["UUID"], "Path": "Assets/Models/TexturedCube.glb#material/0"} in pillars, "scene did not persist the imported material reference: " + json.dumps(pillars))
    # The feature scene's imported-material Pillar resolves: material.create copies what it draws.
    copied = command("material.create", {"path": "Assets/Materials/PillarCopy.bmaterial", "entity": 16})
    require("warnings" not in copied, "copy of the imported material: " + json.dumps(copied))
    copy_file = json.loads((project / "Assets/Materials/PillarCopy.bmaterial").read_text())
    require(copy_file["AlbedoMap"] == {"UUID": model_meta["UUID"], "Path": "Assets/Models/TexturedCube.glb#image/0"} and copy_file["Roughness"] == 0.6, "copied imported material: " + json.dumps(copy_file))
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
