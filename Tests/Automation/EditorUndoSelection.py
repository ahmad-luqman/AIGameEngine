"""Undo/selection checks shared with the real editor viewport integration harness."""
from pathlib import Path


def check_undo_selection(call, require, output, project):
    call("scene.new", {"empty": True})
    require(call("editor.history")["entries"] == [], "New scene did not reset history")
    a = call("entity.create", {"name": "Left", "components": {"Mesh": {"Mesh": "builtin://Cube"}, "Transform": {"Translation": [-1, 0, 0]}}})["id"]
    b = call("entity.create", {"name": "Right", "components": {"Mesh": {"Mesh": "builtin://Cube"}, "Transform": {"Translation": [1, 0, 0]}}})["id"]
    require(call("editor.history")["undo"] == "Create Entity", "Command label is missing")
    selected = call("editor.select", {"entities": [a, b, a]})
    require(selected["entities"] == [a, b] and selected["selected"] == b, "Selection order/deduplication failed")
    call("editor.select", {"entities": [a, "DoesNotExist"]}, error="no entity named")
    require(call("editor.component_set", {"component": "Transform", "data": {"Scale": [1, 1, 1]}})["entities"] == [a, b], "Failed selection changed the active set")
    initial = call("editor.history")
    call("editor.transaction", {"action": "begin", "name": "Move Both"})
    call("editor.transaction", {"action": "begin", "name": "Nested"}, error="already active")
    delta = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, .25, 0, 0, 1]
    for _ in range(4):
        call("editor.transform", {"delta": delta})
    require(call("editor.history")["entries"] == initial["entries"], "Intermediate updates made undo entries")
    call("play.start", error="active transaction")
    call("scene.new", {"empty": True}, error="active transaction")
    call("scene.save", {"path": "Assets/Scenes/Forbidden.bscene"}, error="active transaction")
    call("editor.undo", error="nothing to undo")
    committed = call("editor.transaction", {"action": "commit"})
    require(committed["undo"] == "Move Both" and len(committed["entries"]) == len(initial["entries"]) + 1, "Group transaction granularity failed")

    def pos(entity):
        return call("entity.get", {"entity": entity})["Components"]["Transform"]["Translation"]

    require(pos(a) == [0, 0, 0] and pos(b) == [2, 0, 0], "Group translation failed")
    call("editor.undo")
    require(pos(a) == [-1, 0, 0] and pos(b) == [1, 0, 0], "Group undo was partial")
    require(call("editor.history")["redo"] == "Move Both", "Redo label lost")
    # A cancelled/no-op edit preserves the redo branch, as do selection and read-only commands.
    call("editor.transaction", {"action": "begin", "name": "Cancel"})
    call("editor.transform", {"delta": delta})
    call("editor.transaction", {"action": "cancel"})
    require(pos(a) == [-1, 0, 0] and call("editor.history")["canRedo"], "Cancellation changed state or redo")
    call("editor.transaction", {"action": "begin", "name": "No-op"})
    call("editor.transaction", {"action": "commit"})
    require(call("editor.history")["canRedo"], "No-op transaction discarded redo")
    call("editor.redo")
    require(pos(a) == [0, 0, 0] and pos(b) == [2, 0, 0], "Redo failed")
    call("editor.undo")
    call("editor.component_set", {"component": "Transform", "data": {"Translation": [2, 0, 0]}})
    require(call("editor.history")["undo"] == "Set Transform Translation", "Shared field label missing")
    require(not call("editor.history")["canRedo"], "New edit retained obsolete redo")
    call("editor.undo")
    require(pos(a) == [-1, 0, 0] and pos(b) == [1, 0, 0], "Shared field undo failed")

    call("editor.camera", {"position": [0, 0, 5], "target": [0, 0, 0]})
    view = call("editor.viewport", {"operation": "None", "selectionOutline": True})
    width, height = view["logicalWidth"], view["logicalHeight"]
    selection = call("editor.box_select", {"start": [0, 0], "end": [width / 2, height]})
    require(selection["entities"] == [a], f"Left box selected {selection}")
    selection = call("editor.box_select", {"start": [width, height], "end": [width / 2, 0], "mode": "add"})
    require(selection["entities"] == [a, b], f"Reversed additive box selected {selection}")
    call("editor.box_select", {"start": [-1, 0], "end": [width, height]}, error="outside")
    call("editor.vertex_snap", {"source": [0, 0], "target": [0, 0]}, error="exactly one")
    call("editor.screenshot", {"path": str(Path(output, "multi-selection.png")), "width": 640, "height": 480})
    call("editor.window_screenshot", {"path": str(Path(output, "multi-selection-window.png"))})
    call("editor.viewport")
    # Selected ancestors subsume descendants for tree operations and transforms.
    child = call("entity.create", {"name": "Child", "parent": a})["id"]
    call("editor.select", {"entities": [child, a, b]})
    before_delete = call("scene.get")
    call("editor.delete")
    require(call("scene.info")["entities"] == 0, "Group deletion missed a tree")
    call("editor.undo")
    require(call("scene.get") == before_delete, "Deletion undo changed hierarchy order or contents")
    call("editor.select", {"entities": [child, a, b]})
    copies = call("editor.duplicate")["entities"]
    require(len(copies) == 2, "Duplicate copied selected descendants twice")
    require(call("scene.info")["entities"] == 6, "Duplicate tree count is wrong")
    call("editor.undo")
    require(call("scene.get") == before_delete, "Duplicate undo did not restore scene")
    # Destroy a selected entity from normal runtime updates, without command-driven pruning.
    script = Path(project, "Assets/Scripts/DeleteSelected.lua")
    script.write_text("local S = {}\nfunction S:OnUpdate(dt) self.Entity:Destroy() end\nreturn S\n")
    call("component.set", {"entity": a, "component": "Script", "data": {"Script": "Assets/Scripts/DeleteSelected.lua"}})
    call("editor.select", {"entities": [a, b]})
    call("play.start")
    import time
    time.sleep(.25)
    call("editor.window_screenshot", {"path": str(Path(output, "runtime-pruned-selection.png"))})
    call("entity.get", {"entity": a}, error="no entity with ID")
    call("editor.transaction", {"action": "begin", "name": "Invalid"}, error="edit mode")
    call("editor.transform", {"delta": delta}, error="edit mode")
    call("editor.component_set", {"component": "Transform", "data": {}}, error="edit mode")
    call("editor.box_select", {"start": [0, 0], "end": [width, height]}, error="edit mode")
    call("play.stop")
    call("scene.new", {"empty": True})
    require(call("editor.history")["entries"] == [], "Scene switch retained old patches")
