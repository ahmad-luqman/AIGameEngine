"""Game view checks through the editor's public commands: resolution, letterbox, modes and captures."""
import time
from pathlib import Path

from LocalShadows import read_png


def wait_for(call, require, predicate, message, timeout=15):
    """Polls editor.game_view until predicate(state) holds: the view changes over the next frames."""
    deadline = time.monotonic() + timeout
    while True:
        state = call("editor.game_view")
        if predicate(state):
            return state
        require(time.monotonic() < deadline, f"{message}: {state}")
        time.sleep(.05)


def check_game_view(call, require, output):
    call("scene.new", {"empty": True})
    state = call("editor.game_view", {"resolution": None})
    require(state["mode"] == "Edit" and not state["hasCamera"], f"empty scene: {state}")
    call("editor.game_view", {"path": str(Path(output, "game-none.png"))}, error="no primary camera")
    call("editor.game_view", {"resolution": [16, 16]}, error="resolution")

    camera = call("entity.create", {"name": "GameCamera"})["id"]
    call("component.set", {"entity": camera, "component": "Transform", "data": {"Translation": [0, 0, 6]}})
    call("component.set", {"entity": camera, "component": "Camera", "data": {"Primary": True}})
    cube = call("entity.create", {"name": "GameCube"})["id"]
    call("component.set", {"entity": cube, "component": "Mesh", "data": {"Mesh": "builtin://Cube"}})
    call("component.set", {"entity": cube, "component": "Material", "data": {"AlbedoColor": [1, 0, 0, 1], "EmissiveColor": [1, 0, 0], "EmissiveIntensity": 2}})
    # The editor camera looks at the cube from the side, so the two views differ.
    call("editor.camera", {"position": [7, 2, 0], "target": [0, 0, 0]})

    call("project.set", {"windowWidth": 640, "windowHeight": 360, "graphics": {"RenderScale": 1}})
    state = call("editor.game_view", {"focus": True})
    require(state["width"] == 640 and state["height"] == 360 and state["override"] is None, f"project resolution: {state}")
    state = wait_for(call, require, lambda s: s["visible"] and s["imageRect"]["width"] > 1 and s["renderWidth"] == 640, "the Game view did not come to the front")
    require(state["hasCamera"] and state["mode"] == "Edit", f"edit-mode preview: {state}")
    rect = state["imageRect"]
    require(abs(rect["width"] / rect["height"] - 16 / 9) < .01, f"letterbox lost the aspect ratio: {state}")
    panel = state["panelRect"]
    require(rect["x"] >= panel["x"] - .5 and rect["y"] >= panel["y"] - .5 and rect["x"] + rect["width"] <= panel["x"] + panel["width"] + .5 and rect["y"] + rect["height"] <= panel["y"] + panel["height"] + .5, f"the image leaves the panel: {state}")

    # The Game view's image is the primary camera at the game resolution, as render.screenshot sees it.
    game = Path(output, "game-edit.png")
    call("editor.game_view", {"path": str(game)})
    width, height, pixels = read_png(game)
    require((width, height) == (640, 360), f"Game view capture is {width}x{height}")
    centre = pixels[(height // 2) * width + width // 2]
    require(centre[0] > 120 and centre[0] > 2 * centre[1], f"the cube is not in the middle of the game image: {centre}")
    reference = Path(output, "game-reference.png")
    call("render.screenshot", {"path": str(reference), "width": 640, "height": 360})
    call("image.compare", {"actual": str(game), "reference": str(reference), "assertMatch": True})
    side = Path(output, "game-editor-camera.png")
    call("editor.screenshot", {"path": str(side), "width": 640, "height": 360})
    require(not call("image.compare", {"actual": str(game), "reference": str(side)})["matches"], "the Game view showed the editor camera")

    # The window shows letterbox bars around the image (black) and the game inside it.
    window = Path(output, "game-window.png")
    call("editor.window_screenshot", {"path": str(window)})
    state = call("editor.game_view")
    window_width, window_height, window_pixels = read_png(window)
    scale = window_width / state["windowSize"][0]
    rect, panel = state["imageRect"], state["panelRect"]

    def sample(x, y):
        return window_pixels[int(y * scale) * window_width + int(x * scale)]

    centre = sample(rect["x"] + rect["width"] / 2, rect["y"] + rect["height"] / 2)
    require(centre[0] > 100 and centre[0] > 2 * centre[1], f"the window does not show the game image: {centre}")
    if rect["x"] - panel["x"] > 4:
        bar = sample(panel["x"] + 2, rect["y"] + rect["height"] / 2)
    else:
        require(rect["y"] - panel["y"] > 4, f"no letterbox bar in a 16:9 image of a non-16:9 panel: {state}")
        bar = sample(rect["x"] + rect["width"] / 2, panel["y"] + 2)
    require(max(bar) < 10, f"the letterbox bar is not black: {bar}")

    # A session override changes the resolution without touching the project.
    state = call("editor.game_view", {"resolution": [360, 640]})
    require(state["width"] == 360 and state["height"] == 640 and state["override"] == [360, 640], f"override: {state}")
    state = wait_for(call, require, lambda s: abs(s["imageRect"]["width"] / max(s["imageRect"]["height"], 1) - 360 / 640) < .01, "portrait letterbox")
    portrait = Path(output, "game-portrait.png")
    call("editor.game_view", {"path": str(portrait)})
    require(read_png(portrait)[:2] == (360, 640), "the portrait capture has the wrong size")
    require(call("project.info")["windowWidth"] == 640, "the override changed the project")
    call("editor.game_view", {"resolution": None})

    # Play runs the game in the Game view, brought forward from behind the scene view; the scene view keeps the
    # editor camera, and Stop returns to it.
    call("editor.panel", {"name": "Viewport", "focus": True})
    wait_for(call, require, lambda s: not s["visible"], "the scene view did not come to the front")
    call("play.start")
    state = wait_for(call, require, lambda s: s["mode"] == "Play" and s["visible"], "Play did not bring the Game view forward")
    played = Path(output, "game-play.png")
    call("editor.game_view", {"path": str(played)})
    call("image.compare", {"actual": str(played), "reference": str(game), "assertMatch": True})
    view = call("editor.viewport")
    call("editor.pick", {"x": view["width"] // 2, "y": view["height"] // 2, "select": False})
    call("play.stop")
    wait_for(call, require, lambda s: s["mode"] == "Edit" and not s["visible"], "Stop did not return to the scene view")
    call("entity.destroy", {"entity": camera})
    require(not call("editor.game_view")["hasCamera"], "a destroyed camera still counts as primary")
