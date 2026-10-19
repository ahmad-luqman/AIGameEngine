"""UI authoring selection uses the same transformed, clipped geometry as drawing."""
from pathlib import Path


def check_ui(call, require, output):
    call("scene.new", {"empty": True})
    call("editor.viewport", {"operation": "Translate", "uiBounds": True})
    view = call("editor.viewport")
    width, height = view["width"], view["height"]

    def pick(dx, dy):
        return call("editor.pick", {"x": int(width / 2 + dx * height / 1080),
                                    "y": int(height / 2 + dy * height / 1080),
                                    "select": False})["entity"]

    cube = call("entity.create", {"name": "PickThroughHUD"})["id"]
    call("component.set", {"entity": cube, "component": "Mesh", "data": {"Mesh": "builtin://Cube"}})
    call("editor.camera", {"position": [0, 0, 5], "target": [0, 0, 0]})
    hud = call("entity.create", {"name": "HUDContainer"})["id"]
    call("component.set", {"entity": hud, "component": "UIRect", "data": {"AnchorMin": [0, 0], "AnchorMax": [1, 1], "Size": [0, 0]}})
    call("editor.viewport", {"uiBounds": False})
    require(pick(0, 0) == cube, "HUD container stole world picking with UI Bounds off")
    call("editor.viewport", {"uiBounds": True})
    require(pick(0, 0) == hud, "UI Bounds did not enable HUD container picking")
    call("entity.destroy", {"entity": hud})
    call("entity.destroy", {"entity": cube})

    panel = call("entity.create", {"name": "RotatedPanel"})["id"]
    call("component.set", {"entity": panel, "component": "UIRect",
                           "data": {"Size": [200, 60], "Rotation": 90, "Scale": [1.5, 1]}})
    require(pick(0, 100) == panel, "UI picking missed rotated/scaled rect")
    require(pick(80, 0) == 0, "UI picking used the untransformed rect")
    call("component.set", {"entity": panel, "component": "UIRect",
                           "data": {"Size": [200, 200], "Rotation": 0, "Scale": [1, 1], "ClipChildren": True}})
    child = call("entity.create", {"name": "ClippedChild", "parent": panel})["id"]
    call("component.set", {"entity": child, "component": "UIRect",
                           "data": {"Position": [100, 0], "Size": [100, 100]}})
    require(pick(80, 0) == child, "UI picking did not prefer the topmost child")
    require(pick(120, 0) == 0, "UI picking selected clipped child content")
    call("editor.select", {"entity": child})
    call("editor.window_screenshot", {"path": str(Path(output, "ui-handles.png"))})
