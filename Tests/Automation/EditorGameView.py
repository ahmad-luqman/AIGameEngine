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
    call("component.set", {"entity": cube, "component": "RigidBody", "data": {"Type": "Static"}})
    call("component.set", {"entity": cube, "component": "BoxCollider", "data": {}})
    # The editor camera looks at the cube from the side, so the two views differ.
    call("editor.camera", {"position": [7, 2, 0], "target": [0, 0, 0]})

    call("project.set", {"windowWidth": 640, "windowHeight": 360, "graphics": {"RenderScale": 1}})
    state = call("editor.game_view", {"focus": True})
    require(state["width"] == 640 and state["height"] == 360 and state["override"] is None, f"project resolution: {state}")
    state = wait_for(call, require, lambda s: s["visible"] and s["imageRect"]["width"] > 1 and s["renderWidth"] == 640 and s["status"] == "rendered", "the Game view did not come to the front")
    require(state["hasCamera"] and state["mode"] == "Edit", f"edit-mode preview: {state}")
    project_before_stats = call("project.info")
    state = call("editor.game_view", {"stats": True})
    require(state["stats"] and not state["statsVisible"], f"the overlay must stay hidden in Edit: {state}")
    require(state["physicsBodies"] is None, f"Edit must not claim a measured physics body count: {state}")
    require(call("project.info") == project_before_stats, "stats changed the project")
    call("editor.game_view", {"stats": False})
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
    # render.compare renders and compares in one step.
    compared = call("render.compare", {"reference": str(game), "output": str(Path(output, "game-compare.png")), "width": 640, "height": 360, "assertMatch": True})
    require(compared["similarity"] > .99 and Path(compared["actual"]) == Path(output, "game-compare.png"), f"render.compare: {compared}")
    compared = call("render.compare", {"reference": str(game), "output": str(Path(output, "game-compare-small.png")), "width": 320, "height": 180})
    require(not compared["sizeMatches"] and compared["similarity"] == 0, f"render.compare at another size: {compared}")
    side = Path(output, "game-editor-camera.png")
    call("editor.screenshot", {"path": str(side), "width": 640, "height": 360})
    require(not call("image.compare", {"actual": str(game), "reference": str(side)})["matches"], "the Game view showed the editor camera")

    # A session override changes the resolution without touching the project.
    state = call("editor.game_view", {"resolution": [360, 640]})
    require(state["width"] == 360 and state["height"] == 640 and state["override"] == [360, 640], f"override: {state}")
    state = wait_for(call, require, lambda s: abs(s["imageRect"]["width"] / max(s["imageRect"]["height"], 1) - 360 / 640) < .01, "portrait letterbox")
    portrait = Path(output, "game-portrait.png")
    call("editor.game_view", {"path": str(portrait)})
    require(read_png(portrait)[:2] == (360, 640), "the portrait capture has the wrong size")
    require(call("project.info")["windowWidth"] == 640, "the override changed the project")

    # A portrait game in the (landscape) panel: black bars left and right, the game image between them.
    window = Path(output, "game-window.png")
    call("editor.window_screenshot", {"path": str(window)})
    state = call("editor.game_view")
    window_width, window_height, window_pixels = read_png(window)
    scale = window_width / state["windowSize"][0]
    rect, panel = state["imageRect"], state["panelRect"]
    require(rect["x"] - panel["x"] > 8, f"no letterbox bars beside a portrait image: {state}")

    def sample(x, y):
        return window_pixels[int(y * scale) * window_width + int(x * scale)]

    centre = sample(rect["x"] + rect["width"] / 2, rect["y"] + rect["height"] / 2)
    require(centre[0] > 100 and centre[0] > 2 * centre[1], f"the window does not show the game image: {centre}")
    bar = sample(panel["x"] + 4, rect["y"] + rect["height"] / 2)
    require(max(bar) < 10, f"the letterbox bar is not black: {bar}")
    call("editor.game_view", {"resolution": None})

    # Play runs the game in the Game view, brought forward from behind the scene view; the scene view keeps the
    # editor camera, and Stop returns to it.
    call("editor.panel", {"name": "Viewport", "focus": True})
    wait_for(call, require, lambda s: not s["visible"] and s["status"] == "notRendered", "the scene view did not come to the front")
    subscription = call("events.subscribe", {"types": ["playState"]})
    require(subscription["streamed"], f"the editor's TCP connection did not stream events: {subscription}")
    call("play.start")
    state = wait_for(call, require, lambda s: s["mode"] == "Play" and s["visible"], "Play did not bring the Game view forward")
    # Diagnostics draw over the game without changing its game-image captures.
    require(not state["statsVisible"], f"stats unexpectedly enabled: {state}")
    state = call("editor.game_view", {"stats": True})
    state = wait_for(call, require, lambda s: s["statsVisible"] and s["imguiGpu"]["timers"] and s["imguiGpu"]["frames"] > 0
                     and s["renderStats"] is not None and s["renderStats"]["gpu"]["frames"] > 0,
                     "play diagnostics did not receive GPU timings")
    require(state["renderStats"]["triangles"] == 12, f"one cube should submit 12 color triangles: {state}")
    require(state["physicsBodies"] == 1 and state["luaBytes"] > 0 and state["frameMs"] > 0, f"play diagnostics counters: {state}")
    overlay = Path(output, "game-stats-window.png")
    call("editor.window_screenshot", {"path": str(overlay)})
    state = call("editor.game_view")  # waits for the preceding frame's capture
    width, _, pixels = read_png(overlay)
    scale = width / state["windowSize"][0]
    rect = state["imageRect"]
    # Text in the overlay's first line, against its dark translucent background.
    bright = 0
    for y in range(int((rect["y"] + 14) * scale), int((rect["y"] + 30) * scale)):
        for x in range(int((rect["x"] + 14) * scale), int((rect["x"] + 130) * scale)):
            if min(pixels[y * width + x][:3]) > 160:
                bright += 1
    require(bright > 30, "the enabled play stats overlay did not draw its frame-time text")
    played = Path(output, "game-play.png")
    call("editor.game_view", {"path": str(played)})
    call("image.compare", {"actual": str(played), "reference": str(game), "assertMatch": True})
    require(call("editor.game_view", {"stats": False})["stats"] is False, "stats did not turn off")
    wait_for(call, require, lambda s: not s["statsVisible"], "stats did not disappear")
    # The game UI maps the mouse onto the letterboxed image, not the whole panel (the main window starts at 0,0).
    state = call("editor.game_view")
    host = call("ui.layout")["view"]
    rect = state["imageRect"]
    require(abs(host["origin"][0] - rect["x"]) < 1 and abs(host["origin"][1] - rect["y"]) < 1, f"UI origin {host} vs image {rect}")
    require(abs(host["size"][0] - rect["width"]) < 1 and abs(host["size"][1] - rect["height"]) < 1, f"UI size {host} vs image {rect}")
    # The scene view keeps the editor camera in Play: with the game camera turned away from the cube, the scene
    # view's centre still shows (and picks) it.
    call("component.set", {"entity": camera, "component": "Transform", "data": {"Rotation": [0, 180, 0]}})
    view = call("editor.viewport")
    picked = call("editor.pick", {"x": view["width"] // 2, "y": view["height"] // 2, "select": False})
    require(picked["entity"] == cube, f"the scene view did not keep the editor camera in Play: {picked}")
    call("play.stop")
    wait_for(call, require, lambda s: s["mode"] == "Edit" and not s["visible"], "Stop did not return to the scene view")
    # The play-state events arrived on this connection between the responses.
    states = [event["state"] for event in call.events if event.get("event") == "playState" and event.get("subscription") == subscription["subscription"]]
    require(states == ["play", "stop"], f"streamed play-state events: {call.events}")
    call("events.unsubscribe", {"subscription": subscription["subscription"]})
    require(not call("editor.game_view")["statsVisible"], "the overlay survived Stop")
    call("entity.destroy", {"entity": camera})
    require(not call("editor.game_view")["hasCamera"], "a destroyed camera still counts as primary")

    # Without a camera the Game view says so, and still shows the game UI (a menu scene may have none).
    menu = call("entity.create", {"name": "GameMenu"})["id"]
    call("component.set", {"entity": menu, "component": "UIRect", "data": {"AnchorMin": [.5, .5], "AnchorMax": [.5, .5], "Pivot": [.5, .5], "Position": [0, 0], "Size": [160, 120]}})
    call("component.set", {"entity": menu, "component": "UIImage", "data": {"Color": [0, 0, 1, 1]}})
    call("editor.panel", {"name": "Game", "focus": True})
    wait_for(call, require, lambda s: s["visible"] and s["status"] == "noCamera", "the Game view did not report the missing camera")
    window = Path(output, "game-no-camera.png")
    call("editor.window_screenshot", {"path": str(window)})
    # The screenshot is written at the end of the frame: the next request waits for it.
    state = call("editor.game_view")
    window_width, _, window_pixels = read_png(window)
    scale = window_width / state["windowSize"][0]
    rect = state["imageRect"]
    # Below the centred "No primary camera" message, still inside the panel.
    x, y = rect["x"] + rect["width"] / 2 - 10, rect["y"] + rect["height"] / 2 + 22
    pixel = window_pixels[int(y * scale) * window_width + int(x * scale)]
    require(pixel[2] > 150 and pixel[0] < 80, f"the game UI is missing without a camera: {pixel}")
    call("entity.destroy", {"entity": menu})
