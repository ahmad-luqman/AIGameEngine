"""Graph documents and property key authoring through the editor's public automation paths."""
import json
from pathlib import Path
import shutil


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
    # A track on another entity: capture and scrubbing both change that entity, and both undo.
    lamp = call("entity.create", {"name": "AuthoredLamp"})["id"]
    call("component.set", {"entity": lamp, "component": "PointLight", "data": {"Intensity": 1}})
    call("animation.key", {"entity": entity, "component": "PointLight", "field": "Intensity", "time": 0, "target": "AuthoredLamp"})
    call("component.set", {"entity": lamp, "component": "PointLight", "data": {"Intensity": 3}})
    call("animation.key", {"entity": entity, "component": "PointLight", "field": "Intensity", "time": 1, "target": lamp})
    tracks = call("component.get", {"entity": entity, "component": "PropertyAnimation"})["Tracks"]
    require(len(tracks) == 2 and tracks[1]["Target"] == lamp and len(tracks[1]["Keys"]) == 2, f"Target track was not captured: {tracks}")
    call("animation.sample", {"entity": entity, "time": .5})
    require(call("component.get", {"entity": lamp, "component": "PointLight"})["Intensity"] == 2, "A sample did not reach the target entity")
    call("editor.undo")
    require(call("component.get", {"entity": lamp, "component": "PointLight"})["Intensity"] == 3, "Undo did not restore the target entity")
    call("editor.panel", {"name": "Property Animation", "focus": True})
    call("editor.window_screenshot", {"path": str(Path(output, "property-animation.png"))})
    call("editor.animation_graph", {"close": True})
    check_joint_assets(call, require, output)


def check_joint_assets(call, require, output):
    """The mask and joint map windows: what they open, what they read, and that they follow their files."""
    project = Path(call("project.info")["directory"])
    data = Path(__file__).resolve().parents[1] / "Data/Animation"
    for name in ("Biped.gltf", "UnrealRig.gltf", "MixamoRig.gltf"):
        shutil.copyfile(data / name, project / "Assets" / name)
    call("asset.refresh", {})
    call("scene.new", {"empty": True})

    # Masks: the Animator that uses the file names its model.
    mask = "Assets/Upper.banimmask"
    call("asset.write", {"path": mask, "content": json.dumps({"Joints": [{"Joint": "Spine", "Weight": 0.75}, {"Joint": "Gone"}]})})
    rig = call("entity.create", {"name": "MaskRig"})["id"]
    call("component.set", {"entity": rig, "component": "Animator", "data": {"Mesh": "Assets/Biped.gltf", "Layers": [{"Name": "Upper", "MaskAsset": mask}]}})
    state = call("editor.animation_mask", {"path": mask})
    require(state["open"] and state["valid"] and not state["dirty"] and state["joints"] == 2 and state["model"] == "Assets/Biped.gltf", f"Mask did not open: {state}")
    require(state["missing"] == ["Gone"], f"A joint the model lacks was not reported: {state}")
    call("editor.window_screenshot", {"path": str(Path(output, "animation-mask.png"))})
    call("editor.animation_mask", {"path": "../Upper.banimmask"}, error="is not a .banimmask file under Assets/")
    call("editor.animation_mask", {"path": "Assets/Upper.bjointmap"}, error="is not a .banimmask file under Assets/")
    require(not call("editor.animation_mask", {"model": ""})["model"], "The model did not clear")
    bad_model = call("editor.animation_mask", {"model": "Assets/Gone.gltf"})
    require("Cannot load the model" in bad_model["modelError"], f"A bad model was not reported: {bad_model}")
    call("editor.animation_mask", {"model": "Assets/Biped.gltf"})
    call("asset.move", {"from": mask, "to": "Assets/Moved.banimmask"})
    require(call("editor.animation_mask", {})["path"] == "Assets/Moved.banimmask", "Open mask did not follow asset.move")
    call("asset.move", {"from": "Assets/Biped.gltf", "to": "Assets/Hero.gltf"})
    require(call("editor.animation_mask", {})["model"] == "Assets/Hero.gltf", "The mask's model did not follow asset.move")
    mask_file = project / "Assets/Moved.banimmask"
    mask_file.write_text("{}", encoding="utf-8")
    call("asset.refresh", {"paths": ["Assets/Moved.banimmask"]})
    broken = call("editor.animation_mask", {})
    require(broken["error"] and broken["externalChange"], f"An invalid reload was not reported: {broken}")
    mask_file.write_text(json.dumps({"Joints": [{"Joint": "Chest"}]}), encoding="utf-8")
    call("asset.refresh", {"paths": ["Assets/Moved.banimmask"]})
    fixed = call("editor.animation_mask", {})
    require(not fixed["error"] and not fixed["externalChange"] and fixed["joints"] == 1 and not fixed["missing"], f"A fixed mask kept a stale state: {fixed}")
    require(not call("editor.animation_mask", {"close": True})["open"], "The mask window did not close")

    # Joint maps: the library that names the file gives both models, and the report reads the unsaved pairs.
    path = "Assets/Rig.bjointmap"
    call("asset.write", {"path": path, "content": json.dumps({"Joints": [{"Source": "mixamorig:LeftArm", "Target": "clavicle_l", "Offset": [0, 0, 25]}]})})
    hero = call("entity.create", {"name": "MapRig"})["id"]
    call("component.set", {"entity": hero, "component": "Animator", "data": {"Mesh": "Assets/UnrealRig.gltf", "ClipLibraries": [{"Mesh": "Assets/MixamoRig.gltf", "JointMap": path}]}})
    state = call("editor.joint_map", {"path": path})
    require(state["open"] and state["valid"] and state["source"] == "Assets/MixamoRig.gltf" and state["target"] == "Assets/UnrealRig.gltf", f"Joint map did not open: {state}")
    require(state["mapped"] > 10 and not state["warnings"], f"The report did not read the models: {state}")
    listed = call("editor.joint_map", {"listMatches": True})
    require(listed["dirty"] and listed["joints"] == listed["mapped"], f"Automatic matches were not listed: {listed}")
    require(listed["listed"]["added"] == listed["joints"] - 1 and not listed["listed"]["sharedNames"], f"listMatches did not say what it did: {listed}")
    call("editor.window_screenshot", {"path": str(Path(output, "joint-map.png"))})
    call("editor.joint_map", {"path": "Assets/Other.bjointmap"}, error="Save or Revert")
    call("editor.joint_map", {"close": True, "path": path})
    cleared = call("editor.joint_map", {"source": ""})
    require("mapped" not in cleared, f"A report without both models: {cleared}")
    call("editor.joint_map", {"listMatches": True}, error="both models")
    require(not call("editor.joint_map", {"close": True})["open"], "The joint map window did not close")
    # With no file open there is nothing to list into: the closed draft stays clean, and the next open works.
    closed = {"source": "Assets/MixamoRig.gltf", "target": "Assets/UnrealRig.gltf", "listMatches": True}
    call("editor.joint_map", closed, error="needs an open joint map")
    require(not call("editor.joint_map", {"close": True})["dirty"], "listMatches dirtied a closed draft")
    require(call("editor.joint_map", {"path": path})["open"], "A joint map did not open after a refused listMatches")
    call("editor.joint_map", {"close": True})

    # Reloading the project keeps a draft's unsaved edits; another project closes the draft, which belonged to this one.
    directory = call("project.info")["directory"]
    require(call("editor.joint_map", {"path": path, "listMatches": True})["dirty"], "listMatches left the draft clean")
    call("project.open", {"path": directory})
    call("editor.window_screenshot", {"path": str(Path(output, "joint-map-reloaded.png"))})
    require(call("editor.joint_map", {})["dirty"], "Reloading the project dropped the draft's edits")
    other = Path(output, "OtherProject")
    if not other.exists():
        call("project.create", {"path": str(other), "name": "Other"})
    call("project.open", {"path": str(other)})
    call("editor.window_screenshot", {"path": str(Path(output, "joint-map-closed.png"))})
    require(not call("editor.joint_map", {})["open"], "A draft outlived its project")
    call("project.open", {"path": directory})

    # The same report without the editor, from saved files.
    matched = call("animator.match_joints", {"source": "Assets/MixamoRig.gltf", "target": "Assets/UnrealRig.gltf", "jointMap": path})
    require(any(joint["target"] == "clavicle_l" and joint["match"] == "joint map" and joint["offset"] == [0, 0, 25] for joint in matched["joints"]), f"animator.match_joints missed the pair: {matched}")
