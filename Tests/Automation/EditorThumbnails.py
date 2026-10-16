"""Content-browser previews and the inspector's material preview through the editor's public commands."""
import json
import os
import shutil
import time
from pathlib import Path

from LocalShadows import read_png

ASSETS = Path(__file__).resolve().parents[1] / "Data" / "FeatureTest" / "Assets"
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
    expected = {"Assets/Crate.glb": "Model", "Assets/Checker.png": "Image", "Assets/Environments/Sky.hdr": "Environment", "Assets/Orb.bprefab": "Prefab"}

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
        if kind in ("Model", "Prefab"):
            centre, corner = pixels[(height // 2) * width + width // 2], pixels[2 * width + 2]
            require(sum(abs(a - b) for a, b in zip(centre, corner)) > 40, f"{relative}: nothing in the middle of the preview")
    sky = call("editor.thumbnail", {"path": "Assets/Environments/Sky.hdr"})
    require(sky["width"] == 2 * sky["height"] and "HDR" in sky["details"][0], f"the environment keeps its 2:1 aspect: {sky}")
    crate = call("editor.thumbnail", {"path": "Assets/Crate.glb"})
    require(any("triangles" in line for line in crate["details"]), f"model details: {crate}")
    require(call("editor.thumbnail", {"path": "Assets/Orb.bprefab"})["details"] == ["2 entities"], "prefab details")

    # A changed source makes the cached preview stale.
    stamp = time.time() + 10
    os.utime(Path(project, "Assets/Checker.png"), (stamp, stamp))
    require(not call("editor.thumbnail", {"path": "Assets/Checker.png"})["cached"], "a modified image kept its old preview")
    Path(project, "Assets/Scripts/Note.lua").write_text("return {}\n")
    call("editor.thumbnail", {"path": "Assets/Scripts/Note.lua"}, error="has no preview")
    call("editor.thumbnail", {"path": "Assets/Missing.png"}, error="the file is missing")

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

    # The browser and inspector show the previews (rendered over the next frames) without errors.
    call("editor.select", {"entity": ball})
    call("editor.panel", {"name": "Content Browser", "focus": True})
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        call("editor.viewport")
        time.sleep(.05)
    call("editor.window_screenshot", {"path": str(Path(output, "content-browser.png"))})
    call("editor.viewport")
