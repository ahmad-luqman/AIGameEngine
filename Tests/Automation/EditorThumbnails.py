"""Content-browser previews and the inspector's material preview through the editor's public commands."""
import json
import os
import shutil
import time
from pathlib import Path

from LocalShadows import read_png

ASSETS = Path(__file__).resolve().parents[1] / "Data" / "FeatureTest" / "Assets"
# AssetManager logs (once) the material files check_thumbnails breaks on purpose; the editor test allows these.
EXPECTED_ERRORS = ("Material 'Assets/Broken.bmaterial'", "Material 'Assets/Missing.bmaterial'")
PREFAB = {
    "Format": "BasaltPrefab",
    "Version": 1,
    "Entities": [
        {"ID": 1, "Name": "Orb", "Parent": 0, "Components": {
            "Mesh": {"Mesh": "builtin://Sphere"}, "Material": {"AlbedoColor": [0.1, 0.3, 1, 1]}}},
        {"ID": 2, "Name": "Base", "Parent": 1, "Components": {
            "Transform": {"Translation": [0, -0.6, 0], "Scale": [1, 0.2, 1]}, "Mesh": {"Mesh": "builtin://Cube"}}},
    ],
}


def spread(pixels):
    """The largest channel range over the image: a flat image has none."""
    return max(max(p[c] for p in pixels) - min(p[c] for p in pixels) for c in range(3))


def check_thumbnails(call, require, output, project):
    sources = {
        "Assets/Crate.glb": ASSETS / "Models" / "TexturedCube.glb",
        "Assets/Checker.png": ASSETS / "Textures" / "Checker.png",
        "Assets/Environments/Sky.hdr": ASSETS / "Environments" / "Sky.hdr",
    }
    for relative, source in sources.items():
        shutil.copy(source, Path(project, relative))
    Path(project, "Assets/Orb.bprefab").write_text(json.dumps(PREFAB))
    # A material file: the checker on UV1 with a transform, double-sided and masked.
    material = {"Version": 1, "AlbedoMap": "Assets/Checker.png", "AlphaMode": "Mask", "DoubleSided": True,
                "TextureTransforms": {"Albedo": {"Scale": [4, 4], "Rotation": 0.5, "TexCoord": 1}}}
    call("asset.write", {"path": "Assets/Tiles.bmaterial", "content": json.dumps(material)})
    expected = {"Assets/Crate.glb": "Model", "Assets/Checker.png": "Image", "Assets/Environments/Sky.hdr": "Environment", "Assets/Orb.bprefab": "Prefab", "Assets/Tiles.bmaterial": "Material"}

    for relative, kind in expected.items():
        path = Path(output, "thumbnail-" + Path(relative).stem + ".png")
        result = call("editor.thumbnail", {"path": relative, "output": str(path), "refresh": True})
        require(result["kind"] == kind and not result["cached"], f"{relative}: {result}")
        require(0 < result["width"] <= 128 and 0 < result["height"] <= 128, f"{relative}: preview size {result}")
        require(Path(result["cachePath"]).is_file() and Path(project, "Cache", "Thumbnails") in Path(result["cachePath"]).parents, f"{relative}: cache {result}")
        width, height, pixels = read_png(path)
        require((width, height) == (result["width"], result["height"]), f"{relative}: written {width}x{height}")
        require(spread(pixels) > 40, f"{relative}: the preview is flat")
        again = call("editor.thumbnail", {"path": relative})
        require(again["cached"] and again["details"] == result["details"], f"{relative}: second request {again}")
        if kind in ("Model", "Prefab", "Material"):
            centre, corner = pixels[(height // 2) * width + width // 2], pixels[2 * width + 2]
            require(sum(abs(a - b) for a, b in zip(centre, corner)) > 40, f"{relative}: nothing in the middle of the preview")
    sky = call("editor.thumbnail", {"path": "Assets/Environments/Sky.hdr"})
    require(sky["width"] == 2 * sky["height"] and "HDR" in sky["details"][0], f"the environment keeps its 2:1 aspect: {sky}")
    crate = call("editor.thumbnail", {"path": "Assets/Crate.glb"})
    require(any("triangles" in line for line in crate["details"]), f"model details: {crate}")
    require(call("editor.thumbnail", {"path": "Assets/Orb.bprefab"})["details"] == ["2 entities"], "prefab details")
    require(call("editor.thumbnail", {"path": "Assets/Tiles.bmaterial"})["details"] == ["Mask, double-sided, 1 map(s)"], "material details")
    # Rewriting the material (or touching its texture) makes its cached preview stale.
    material["AlphaMode"] = "Opaque"
    call("asset.write", {"path": "Assets/Tiles.bmaterial", "content": json.dumps(material)})
    rewritten = call("editor.thumbnail", {"path": "Assets/Tiles.bmaterial"})
    require(not rewritten["cached"] and rewritten["details"] == ["Opaque, double-sided, 1 map(s)"], f"a rewritten material kept its old preview: {rewritten}")
    call("asset.write", {"path": "Assets/Broken.bmaterial", "content": json.dumps({"Version": 1})})
    Path(project, "Assets/Broken.bmaterial").write_text('{"Version": 1, "Clearcoat": 3}')
    call("editor.thumbnail", {"path": "Assets/Broken.bmaterial", "refresh": True}, error="Clearcoat 3 must be in [0, 1]")
    call("asset.delete", {"path": "Assets/Broken.bmaterial"})

    # A changed source makes the cached preview stale.
    stamp = time.time() + 10
    os.utime(Path(project, "Assets/Checker.png"), (stamp, stamp))
    require(not call("editor.thumbnail", {"path": "Assets/Checker.png"})["cached"], "a modified image kept its old preview")
    Path(project, "Assets/Scripts/Note.lua").write_text("return {}\n")
    call("editor.thumbnail", {"path": "Assets/Scripts/Note.lua"}, error="has no preview")
    call("editor.thumbnail", {"path": "Assets/Missing.png"}, error="the file is missing")
    for outside in ("Assets/../Project.bproject.png", "../Outside.png", "Checker.png"):
        call("editor.thumbnail", {"path": outside}, error="is not inside Assets/")
    # A file with an image extension that is not an image fails with the decoder's reason, and is not cached.
    Path(project, "Assets/Broken.png").write_bytes(b"not a png")
    call("editor.thumbnail", {"path": "Assets/Broken.png"}, error="Broken.png")

    # The inspector's material sphere.
    call("scene.new", {"empty": True})
    ball = call("entity.create", {"name": "RedBall"})["id"]
    call("component.set", {"entity": ball, "component": "Material", "data": {"AlbedoColor": [1, 0.05, 0.05, 1]}})
    sphere = Path(output, "material-preview.png")
    result = call("editor.material_preview", {"entity": "RedBall", "output": str(sphere)})
    require(result["width"] == 128 and result["height"] == 128, f"material preview: {result}")
    width, height, pixels = read_png(sphere)
    centre = pixels[(height // 2) * width + width // 2]
    require(centre[0] > 2 * centre[1] and centre[0] > 2 * centre[2], f"the material preview is not red: {centre}")
    plain = call("entity.create", {"name": "Plain"})["id"]
    call("editor.material_preview", {"entity": plain, "output": str(sphere)}, error="has no Material component")
    # A Material asset that does not load is an error, not the inline fallback.
    call("component.set", {"entity": ball, "component": "Material", "data": {"Material": "Assets/Missing.bmaterial"}})
    call("editor.material_preview", {"entity": "RedBall", "output": str(sphere)}, error="Assets/Missing.bmaterial")
    call("component.set", {"entity": ball, "component": "Material", "data": {"Material": "Assets/Tiles.bmaterial"}})
    call("editor.material_preview", {"entity": "RedBall", "output": str(sphere)})
    call("component.set", {"entity": ball, "component": "Material", "data": {"Material": ""}})

    # The Material Editor shows a material file and follows writes made while it has no unsaved edits.
    state = call("editor.material_editor", {"path": "Assets/Tiles.bmaterial"})
    require(state["open"] and state["path"] == "Assets/Tiles.bmaterial" and not state["dirty"] and state["valid"] and not state["error"], f"material editor: {state}")
    material["Roughness"] = 0.25
    call("asset.write", {"path": "Assets/Tiles.bmaterial", "content": json.dumps(material)})
    require(not call("editor.material_editor", {})["dirty"], "a clean draft did not follow the file")
    call("editor.material_editor", {"path": "Assets/Missing.bmaterial"}, error="cannot read")
    require(call("editor.material_editor", {})["path"] == "Assets/Tiles.bmaterial", "a failed open replaced the draft")

    # The browser, inspector and Material Editor show the previews (rendered over the next frames) without errors.
    call("editor.select", {"entity": ball})
    call("editor.panel", {"name": "Content Browser", "focus": True})
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        call("editor.viewport")
        time.sleep(.05)
    call("editor.window_screenshot", {"path": str(Path(output, "content-browser.png"))})
    require(not call("editor.material_editor", {"close": True})["open"], "the Material Editor did not close")
    call("editor.viewport")
