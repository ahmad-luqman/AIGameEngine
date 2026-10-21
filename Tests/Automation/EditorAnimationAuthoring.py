"""Graph documents and property key authoring through the editor's public automation paths."""
import json
from pathlib import Path


def check_animation_authoring(call, require, output):
    graph = {
        "InitialState": "Idle",
        "Parameters": [{"Name": "Speed", "Type": "Float", "Default": 0}],
        "States": [{"Name": "Idle", "Clip": "Idle"}, {"Name": "Walk", "Clip": "Walk"}],
        "Transitions": [{"From": "Idle", "To": "Walk", "Duration": .2,
                         "Conditions": [{"Parameter": "Speed", "Operator": "Greater", "Value": .5}]}],
    }
    path = "Assets/Authoring.banimgraph"
    call("asset.write", {"path": path, "content": json.dumps(graph)})
    state = call("editor.animation_graph", {"path": path})
    require(state["open"] and state["valid"] and not state["dirty"], f"Graph did not open: {state}")
    call("editor.panel", {"name": "Animation Graph", "focus": True})
    call("editor.window_screenshot", {"path": str(Path(output, "animation-graph.png"))})
    call("scene.new", {"empty": True})
    entity = call("entity.create", {"name": "AuthoredDoor"})["id"]
    call("editor.select", {"entity": entity})
    call("animation.key", {"entity": entity, "component": "Transform", "field": "Translation", "time": 0})
    call("component.set", {"entity": entity, "component": "Transform", "data": {"Translation": [8, 4, 2]}})
    call("animation.key", {"entity": entity, "component": "Transform", "field": "Translation", "time": 1})
    call("animation.sample", {"entity": entity, "time": .5})
    call("assert.component", {"entity": entity, "component": "Transform", "fields": {"Translation": [4, 2, 1]}})
    call("editor.undo")
    call("assert.component", {"entity": entity, "component": "Transform", "fields": {"Translation": [8, 4, 2]}})
    call("editor.redo")
    call("assert.component", {"entity": entity, "component": "Transform", "fields": {"Translation": [4, 2, 1]}})
    call("editor.panel", {"name": "Property Animation", "focus": True})
    call("editor.window_screenshot", {"path": str(Path(output, "property-animation.png"))})
    call("editor.animation_graph", {"close": True})
