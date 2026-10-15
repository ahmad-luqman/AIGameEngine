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
        if sys.platform == "darwin":
            env["DYLD_FALLBACK_LIBRARY_PATH"] = "/opt/homebrew/lib:/usr/local/lib"
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

                    def call(command, params=None, error=False):
                        stream.write((json.dumps({"command": command, "params": params or {}}) + "\n").encode())
                        stream.flush()
                        response = json.loads(stream.readline())
                        require(response.get("ok") != error, f"{command}: {response}")
                        return response.get("result") if not error else response

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
                    call("editor.viewport", {"grid": True, "scaleStep": 0}, error=True)
                    require(call("editor.viewport")["grid"] is False, "Invalid patch was partially applied")
                    width, height = settings["width"], settings["height"]
                    require(width > 16 and height > 16, "Viewport is not visible")
                    # Camera is axis-aligned, 50-degree vertical FOV; both cubes' front faces are z=.5.
                    def project(x, y, z):
                        focal = height / (2 * math.tan(math.radians(25)))
                        return [width / 2 + x * focal / (5 - z), height / 2 - y * focal / (5 - z)]
                    x, y = project(-0.8, 0, .5)
                    require(call("editor.pick", {"x": int(x), "y": int(y)})["entity"] == a, "Mesh pick missed source")
                    call("editor.pick", {"x": width, "y": 0}, error=True)
                    call("editor.screenshot", {"path": str(Path(output, "outlined.png")), "width": 640, "height": 480})
                    call("editor.viewport", {"selectionOutline": False})
                    call("editor.screenshot", {"path": str(Path(output, "plain.png")), "width": 640, "height": 480})
                    require(Path(output, "outlined.png").read_bytes() != Path(output, "plain.png").read_bytes(), "Outline did not change capture")
                    call("editor.viewport", {"selectionOutline": True})
                    call("editor.vertex_snap", {"source": project(-.3, .5, .5), "target": project(.3, .5, .5)})
                    def translation():
                        return call("entity.get", {"entity": a})["Components"]["Transform"]["Translation"]
                    moved = translation()
                    require(abs(moved[0] - (-.2)) < 1e-4, f"Vertex snap moved to {moved}")
                    call("editor.undo")
                    require(abs(translation()[0] + .8) < 1e-4, "Snap did not undo as one edit")
                    call("editor.redo")
                    require(abs(translation()[0] + .2) < 1e-4, "Snap did not redo")
                    call("play.start")
                    call("editor.vertex_snap", {"source": [0, 0], "target": [0, 0]}, error=True)
                    call("editor.pick", {"x": 0, "y": 0}, error=True)
                    call("play.stop")
                    call("editor.viewport", {"operation": "Translate"})
                    call("editor.window_screenshot", {"path": str(Path(output, "window.png"))})
                    call("editor.viewport")
                    errors = call("log.get", {"errorsOnly": True})
                    require(not errors["messages"], f"Editor logged errors: {errors}")
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
