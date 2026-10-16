"""Post-process capture and runtime graphics checks through the editor's public commands."""
import time
from pathlib import Path

from LocalShadows import read_png


def check_post_process(call, require, output):
    call("scene.new", {"empty": True})
    cube = call("entity.create", {"name": "PostCube"})["id"]
    call("component.set", {"entity": cube, "component": "Mesh", "data": {"Mesh": "builtin://Cube"}})
    call("component.set", {"entity": cube, "component": "Material", "data": {"EmissiveColor": [1, 1, 1], "EmissiveIntensity": 8}})
    post = call("entity.create", {"name": "Post"})["id"]
    call("component.set", {"entity": post, "component": "PostProcess", "data": {"AutoExposure": True, "BloomEnabled": True, "FXAAEnabled": True}})
    call("editor.camera", {"position": [0, 0, 5], "target": [0, 0, 0]})
    call("editor.select", {})
    call("editor.viewport", {"grid": False, "selectionOutline": False})

    def capture(name):
        path = Path(output, name + ".png")
        call("editor.screenshot", {"path": str(path), "width": 320, "height": 240})
        return read_png(path)[2]

    initial = capture("post-initial")
    call("component.set", {"entity": cube, "component": "Material", "data": {"EmissiveIntensity": .05}})
    require(capture("post-dark") != initial, "Exposure/bloom did not respond to the changed scene")
    call("editor.camera", {"position": [0, 0, 2], "target": [0, 0, 0]})
    view = call("editor.viewport")
    call("editor.pick", {"x": view["width"] // 2, "y": view["height"] // 2, "select": False})
    call("component.set", {"entity": cube, "component": "Material", "data": {"EmissiveIntensity": 8}})
    call("editor.camera", {"position": [0, 0, 5], "target": [0, 0, 0]})
    require(capture("post-restored") == initial, "Screenshot exposure inherited an earlier capture/pick")

    call("project.set", {"graphics": {"RenderScale": 1, "DynamicResolution": False}})
    call("play.start")
    call("lua.exec", {"code": "Graphics.SetSettings({RenderScale=0.5,VSync=false,ShadowQuality='Low',SSAOQuality='Off'})"})
    deadline = time.monotonic() + 15
    while True:
        view = call("editor.viewport")
        if view["renderWidth"] == max(1, int(view["width"] * .5 + .5)):
            break
        require(time.monotonic() < deadline, "Lua graphics scale did not reach editor renderer")
        time.sleep(.05)
    require(call("project.info")["graphics"]["RenderScale"] == 1, "Lua graphics saved project defaults")
    call("play.stop")
    deadline = time.monotonic() + 15
    while call("editor.viewport")["renderWidth"] != view["width"]:
        require(time.monotonic() < deadline, "Stopping Play did not restore project graphics scale")
        time.sleep(.05)
