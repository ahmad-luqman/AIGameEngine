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
    require(state["open"] and state["valid"] and not state["dirty"] and not state["externalChange"] and not state["error"], f"Graph did not open: {state}")
    call("editor.panel", {"name": "Animation Graph", "focus": True})
    call("editor.window_screenshot", {"path": str(Path(output, "animation-graph.png"))})
    call("asset.move", {"from": path, "to": "Assets/Moved.banimgraph"})
    require(call("editor.animation_graph")["path"] == "Assets/Moved.banimgraph", "Open graph did not follow asset.move")
    call("editor.animation_graph", {"path": "../Authoring.banimgraph"}, error="open a .banimgraph")
    call("editor.animation_graph", {"path": "Assets/Other.lua"}, error="open a .banimgraph")
    graph_file = Path(call("project.info")["directory"], "Assets/Moved.banimgraph")
    graph_file.write_text("{}", encoding="utf-8")
    call("asset.refresh", {"paths": ["Assets/Moved.banimgraph"]})
    broken = call("editor.animation_graph")
    require(broken["error"] and broken["externalChange"], f"Invalid graph reload was not reported: {broken}")
    graph_file.write_text(json.dumps(graph), encoding="utf-8")
    call("asset.refresh", {"paths": ["Assets/Moved.banimgraph"]})
    restored = call("editor.animation_graph")
    require(not restored["error"] and not restored["externalChange"], f"Fixed graph kept a stale error: {restored}")
    call("scene.new", {"empty": True})
    entity = call("entity.create", {"name": "AuthoredDoor"})["id"]
    call("editor.select", {"entity": entity})
    call("animation.key", {"entity": entity, "component": "Transform", "field": "Translation", "time": 0})
    call("editor.undo")
    call("assert.component", {"entity": entity, "component": "PropertyAnimation", "exists": False})
    call("editor.redo")
    require(len(call("component.get", {"entity": entity, "component": "PropertyAnimation"})["Tracks"][0]["Keys"]) == 1, "Redo did not restore first captured key")
    call("component.set", {"entity": entity, "component": "Transform", "data": {"Translation": [8, 4, 2]}})
    call("animation.key", {"entity": entity, "component": "Transform", "field": "Translation", "time": 1})
    call("editor.undo")
    require(len(call("component.get", {"entity": entity, "component": "PropertyAnimation"})["Tracks"][0]["Keys"]) == 1, "Undo did not remove time-1 key")
    call("editor.redo")
    require(len(call("component.get", {"entity": entity, "component": "PropertyAnimation"})["Tracks"][0]["Keys"]) == 2, "Redo did not restore time-1 key")
    call("animation.sample", {"entity": entity, "time": .5})
    call("assert.component", {"entity": entity, "component": "Transform", "fields": {"Translation": [4, 2, 1]}})
    call("editor.undo")
    call("assert.component", {"entity": entity, "component": "Transform", "fields": {"Translation": [8, 4, 2]}})
    call("editor.redo")
    call("assert.component", {"entity": entity, "component": "Transform", "fields": {"Translation": [4, 2, 1]}})
    call("editor.panel", {"name": "Property Animation", "focus": True})
    call("editor.window_screenshot", {"path": str(Path(output, "property-animation.png"))})
    call("editor.animation_graph", {"close": True})
