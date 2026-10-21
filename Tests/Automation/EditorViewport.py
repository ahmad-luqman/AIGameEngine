#!/usr/bin/env python3
"""Exercise editor viewport commands, undo and capture through the real TCP host (requires a display)."""
import json
import math
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Rendering"))
from LocalShadows import read_png
from EditorUndoSelection import check_undo_selection
from EditorPostProcess import check_post_process
from EditorProjectSettings import check_project_settings
from EditorGameView import check_game_view
from EditorThumbnails import EXPECTED_ERRORS, check_thumbnails
from EditorUI import check_ui
from EditorAnimationAuthoring import check_animation_authoring
from EditorRagdolls import check_ragdolls


def require(value, message):
    if not value:
        raise RuntimeError(message)


def main():
    editor, cli, output = map(lambda value: str(Path(value).resolve()), sys.argv[1:4])
    Path(output).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="BasaltViewport-") as temp:
        created = subprocess.run([cli, "project.create", json.dumps({"path": temp})], capture_output=True, text=True, timeout=30)
        require(created.returncode == 0, created.stderr)
        with socket.socket() as reserve:
            reserve.bind(("127.0.0.1", 0))
            port = reserve.getsockname()[1]
        env = dict(os.environ, BASALT_USER_DATA_DIR=str(Path(temp, "UserData")))
        log_path = Path(output, "editor.log")
        with log_path.open("w") as log:
            process = subprocess.Popen([editor, "--project", temp, "--port", str(port)], cwd=temp, stdout=log, stderr=log, env=env)
            try:
                connection = None
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    require(process.poll() is None, "Editor exited during startup")
                    try:
                        connection = socket.create_connection(("127.0.0.1", port), timeout=2)
                        break
                    except OSError:
                        time.sleep(0.1)
                require(connection is not None, "Editor server did not start")
                with connection:
                    connection.settimeout(30)
                    stream = connection.makefile("rwb")

                    events = []

                    def call(command, params=None, error=None):
                        stream.write((json.dumps({"command": command, "params": params or {}}) + "\n").encode())
                        stream.flush()
                        # Frames without "ok" are events of this connection's subscriptions (events.subscribe).
                        while "ok" not in (response := json.loads(stream.readline())):
                            events.append(response)
                        require(response.get("ok") == (error is None), f"{command}: {response}")
                        if error is not None:
                            require(error in response.get("error", ""), f"{command}: expected {error!r}, got {response}")
                        return response.get("result") if error is None else response

                    call.events = events

                    call("scene.new", {"empty": True})
                    a = call("entity.create", {"name": "Source"})["id"]
                    b = call("entity.create", {"name": "Target"})["id"]
                    for identifier, x in [(a, -0.8), (b, 0.8)]:
                        call("component.set", {"entity": identifier, "component": "Mesh", "data": {"Mesh": "builtin://Cube"}})
                        call("component.set", {"entity": identifier, "component": "Transform", "data": {"Translation": [x, 0, 0]}})
                    call("editor.camera", {"position": [0, 0, 5], "target": [0, 0, 0]})
                    settings = call("editor.viewport", {"operation": "None", "space": "World", "snap": True, "translationStep": 0.25,
                                                        "rotationStep": 30, "scaleStep": 0.2, "grid": False, "colliders": False,
                                                        "lightGizmos": False, "cameraGizmos": False, "uiBounds": False, "debugLines": False})
                    require(settings["space"] == "World" and settings["translationStep"] == 0.25, "Viewport patch failed")
                    call("editor.viewport", {"grid": True, "scaleStep": 0}, error="scaleStep")
                    require(call("editor.viewport")["grid"] is False, "Invalid patch was partially applied")
                    width, height = settings["width"], settings["height"]
                    require(width > 16 and height > 16, "Viewport is not visible")
                    # Camera is axis-aligned, 50-degree vertical FOV; both cubes' front faces are z=.5.
                    def project(x, y, z):
                        focal = height / (2 * math.tan(math.radians(25)))
                        return [width / 2 + x * focal / (5 - z), height / 2 - y * focal / (5 - z)]
                    x, y = project(-0.8, 0, .5)
                    require(call("editor.pick", {"x": int(x), "y": int(y)})["entity"] == a, "Mesh pick missed source")
                    call("editor.pick", {"x": width, "y": 0}, error="outside")
                    call("editor.screenshot", {"path": str(Path(output, "outlined.png")), "width": 640, "height": 480})
                    call("editor.viewport", {"selectionOutline": False})
                    call("editor.screenshot", {"path": str(Path(output, "plain.png")), "width": 640, "height": 480})
                    outlined = read_png(Path(output, "outlined.png"))[2]
                    plain = read_png(Path(output, "plain.png"))[2]
                    differences = [p for p, q in zip(outlined, plain) if p != q]
                    require(len(differences) > 100, "Outline is missing")
                    require(all(p[0] == 255 and 190 <= p[1] <= 192 and p[2] <= 26 for p in differences), "Capture changes are not amber outline pixels")
                    call("editor.screenshot", {"path": str(Path(output, "plain-repeat.png")), "width": 640, "height": 480})
                    require(read_png(Path(output, "plain-repeat.png"))[2] == plain, "Disabled outline capture is unstable")
                    call("editor.viewport", {"selectionOutline": True})
                    snap_entries = len(call("editor.history")["entries"])
                    call("editor.vertex_snap", {"source": project(-.3, .5, .5), "target": project(.3, .5, .5)})
                    snap_history = call("editor.history")
                    require(len(snap_history["entries"]) == snap_entries + 1 and snap_history["undo"] == "Vertex Snap", "Vertex snap did not create its own named entry")
                    def translation():
                        return call("entity.get", {"entity": a})["Components"]["Transform"]["Translation"]
                    moved = translation()
                    require(abs(moved[0] - (-.2)) < 1e-4, f"Vertex snap moved to {moved}")
                    call("editor.undo")
                    require(abs(translation()[0] + .8) < 1e-4, "Snap did not undo as one edit")
                    call("editor.redo")
                    require(abs(translation()[0] + .2) < 1e-4, "Snap did not redo")

                    # A batch runs on the main thread with no viewport frame between mutations and picks.
                    def batch(requests):
                        stream.write((json.dumps([{"command": c, "params": p} for c, p in requests]) + "\n").encode())
                        stream.flush()
                        responses = json.loads(stream.readline())
                        require(all(r.get("ok") for r in responses), f"Batch failed: {responses}")
                        return [r["result"] for r in responses]

                    def pick_at(px, py):
                        return {"x": int(px), "y": int(py), "select": False}

                    px, py = project(-.2, 0, .5)
                    query = pick_at(px, py)
                    for component, toggle in [("PointLight", "lightGizmos"), ("Camera", "cameraGizmos"), ("AudioSource", "audioGizmos")]:
                        helper = call("entity.create", {"name": component})["id"]
                        call("component.set", {"entity": helper, "component": component, "data": {}})
                        call("editor.viewport", {toggle: True})
                        results = batch([
                            ("component.set", {"entity": helper, "component": "Transform", "data": {"Translation": [-.2, 0, 2]}}),
                            ("editor.pick", query),
                            ("component.set", {"entity": helper, "component": "Transform", "data": {"Translation": [-.2, 0, -2]}}),
                            ("editor.pick", query),
                        ])
                        require(results[1]["entity"] == helper, f"{component} in front of mesh is not selectable")
                        require(results[3]["entity"] == a, f"Occluded {component} intercepted mesh pick")
                        call("component.set", {"entity": helper, "component": "Transform", "data": {"Translation": [-.2, 0, 2]}})
                        call("editor.viewport", {toggle: False})
                        require(call("editor.pick", query)["entity"] == a, f"Hidden {component} intercepted mesh pick")
                        call("entity.destroy", {"entity": helper})

                    # Source selection survives select=false and failures; target/background queries do not change it.
                    call("editor.select", {"entity": a})
                    qx, qy = project(.8, 0, .5)
                    require(call("editor.pick", pick_at(qx, qy))["entity"] == b, "Target query failed")
                    require(call("editor.pick", {"x": 0, "y": 0, "select": False})["entity"] == 0, "Background query failed")
                    call("editor.vertex_snap", {"source": project(-.7, .5, .5), "target": project(1.3, .5, .5)})
                    require(abs(translation()[0] - 1.8) < 1e-4, "select=false changed the selected source")
                    call("editor.undo")
                    call("component.set", {"entity": a, "component": "Mesh", "data": {"MeshIndex": 999}})
                    call("editor.vertex_snap", {"source": [0, 0], "target": [0, 0]}, error="mesh index is invalid")
                    call("component.set", {"entity": a, "component": "Mesh", "data": {"MeshIndex": 0}})
                    call("editor.vertex_snap", {"source": [0, 0], "target": [0, 0]}, error="no source vertex")
                    call("editor.vertex_snap", {"source": project(.3, .5, .5), "target": [0, 0]}, error="no target vertex")
                    call("editor.vertex_snap", {"source": [-1, 0], "target": [0, 0]}, error="inside the logical viewport")
                    call("editor.select", {"entity": None})
                    call("editor.vertex_snap", {"source": [0, 0], "target": [0, 0]}, error="select a mesh entity")
                    call("component.set", {"entity": a, "component": "UIRect", "data": {}})
                    call("editor.select", {"entity": a})
                    call("editor.vertex_snap", {"source": [0, 0], "target": [0, 0]}, error="select a mesh entity")
                    call("component.remove", {"entity": a, "component": "UIRect"})
                    results = batch([
                        ("component.set", {"entity": a, "component": "Transform", "data": {"Translation": [20, 0, 0]}}),
                        ("editor.pick", query),
                        ("component.set", {"entity": a, "component": "Transform", "data": {"Translation": [-.2, 0, 0]}}),
                        ("editor.pick", query),
                        ("editor.camera", {"position": [20, 0, 5], "target": [20, 0, 0]}),
                        ("editor.pick", query),
                        ("editor.camera", {"position": [0, 0, 5], "target": [0, 0, 0]}),
                        ("entity.destroy", {"entity": a}),
                        ("editor.pick", query),
                    ])
                    require([results[i]["entity"] for i in [1, 3, 5, 8]] == [0, a, 0, 0], "Picking reused stale transform/camera/deletion data")
                    call("editor.pick", {"x": 0, "y": 0})
                    call("editor.vertex_snap", {"source": [0, 0], "target": [0, 0]}, error="select a mesh entity")

                    call("project.set", {"renderScale": .5})
                    deadline = time.monotonic() + 20
                    while True:
                        view = call("editor.viewport")
                        if view["renderWidth"] == int(width * .5 + .5) and view["renderHeight"] == int(height * .5 + .5):
                            break
                        require(time.monotonic() < deadline, "Editor did not display the reduced render scale")
                        time.sleep(.05)
                    thin = call("entity.create", {"name": "Thin"})["id"]
                    call("component.set", {"entity": thin, "component": "Mesh", "data": {"Mesh": "builtin://Cube"}})
                    # Choose a silhouette between a native pixel ray and the displayed reduced-resolution texel ray.
                    # A capture renderer accidentally using the project maximum scale returns the opposite answer.
                    edge_x = width // 2 + 4
                    native_uv = (edge_x + .5) / width
                    render_x = int((edge_x + .5) * view["renderWidth"] / width)
                    displayed_uv = (render_x + .5) / view["renderWidth"]
                    require(abs(native_uv - displayed_uv) > 1e-7, "Test needs distinct native and reduced sample rays")
                    half_uv = (native_uv + displayed_uv) / 2 - .5
                    scale_x = half_uv * 4 * 4.5 * math.tan(math.radians(25)) * width / height
                    call("component.set", {"entity": thin, "component": "Transform", "data": {"Scale": [scale_x, 1, 1]}})
                    sample_xs = list(range(width // 2 - 8, width // 2 + 9))
                    def sample_mask():
                        return [r["entity"] for r in batch([
                            ("editor.pick", {"x": sample_x, "y": height // 2, "select": False})
                            for sample_x in sample_xs])]
                    reduced = sample_mask()
                    call("project.set", {"renderScale": 1})
                    deadline = time.monotonic() + 20
                    while call("editor.viewport")["renderWidth"] != width:
                        require(time.monotonic() < deadline, "Editor did not display native render scale")
                        time.sleep(.05)
                    native = sample_mask()
                    different = [i for i in range(len(sample_xs)) if reduced[i] != native[i]]
                    require(different, f"Thin silhouette must distinguish sampling grids: {view}, {reduced}, {native}")
                    call("project.set", {"renderScale": .5})
                    deadline = time.monotonic() + 20
                    while call("editor.viewport")["renderWidth"] != view["renderWidth"]:
                        require(time.monotonic() < deadline, "Editor did not restore reduced render scale")
                        time.sleep(.05)
                    # Compare observed raster coverage rather than an analytic edge expectation: hardware
                    # subpixel precision and viewport rounding may choose either side of a narrow silhouette.
                    # A queued setting change must still use the displayed grid, not the next frame's grid.
                    index = different[0]
                    transition = batch([("editor.viewport", {}),
                                        ("editor.pick", {"x": sample_xs[index], "y": height // 2, "select": False}),
                                        ("project.set", {"renderScale": 1}),
                                        ("editor.pick", {"x": sample_xs[index], "y": height // 2, "select": False}),
                                        ("editor.viewport", {})])
                    require(transition[0]["renderWidth"] == view["renderWidth"], f"Reduced viewport changed before batch: {transition}")
                    require(transition[1]["entity"] == reduced[index], f"Reduced pick changed before setting update: {transition}, reduced={reduced}, native={native}")
                    require(transition[3]["entity"] == reduced[index],
                            f"Picking used next-frame scale: transition={transition}, reduced={reduced[index]}, native={native[index]}, viewport={view}")
                    call("entity.destroy", {"entity": thin})
                    call("play.start")
                    call("editor.vertex_snap", {"source": [0, 0], "target": [0, 0]}, error="only available in edit mode")
                    # The scene view keeps the editor camera in Play, so picking still works there.
                    require("entity" in call("editor.pick", {"x": 0, "y": 0, "select": False}), "Picking failed in Play")
                    call("play.stop")
                    call("editor.viewport", {"operation": "Translate"})
                    call("editor.window_screenshot", {"path": str(Path(output, "window.png"))})
                    call("editor.viewport")
                    check_undo_selection(call, require, output, temp)
                    check_post_process(call, require, output)
                    check_game_view(call, require, output)
                    check_thumbnails(call, require, output, temp)
                    check_project_settings(call, require, output)
                    check_ui(call, require, output)
                    check_animation_authoring(call, require, output)
                    check_ragdolls(call, require, output)
                    errors = call("log.get", {"errorsOnly": True})
                    unexpected = [m for m in errors["messages"] if not m["text"].startswith(EXPECTED_ERRORS)]
                    require(not unexpected, f"Editor logged errors: {errors}")
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
        log_text = log_path.read_text()
        require("validation enabled" in log_text, "Vulkan validation was not enabled")
        require("Validation Error" not in log_text and "[nvrhi]" not in log_text, "GPU validation reported errors")
    print("Editor viewport integration passed")


if __name__ == "__main__":
    main()
