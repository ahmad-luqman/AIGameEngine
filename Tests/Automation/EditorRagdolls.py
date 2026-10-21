"""Exercise passive ragdolls through the real editor host and capture their collider overlay."""
from pathlib import Path
import shutil

from LocalShadows import read_png


def check_ragdolls(call, require, output):
    project = Path(call("project.info")["directory"])
    source = Path(__file__).resolve().parents[1] / "Data/Animation/TwoJoint.gltf"
    target = project / "Assets/Ragdoll.gltf"
    shutil.copyfile(source, target)
    call("asset.refresh", {"paths": ["Assets/Ragdoll.gltf"]})
    call("scene.new", {"empty": True})
    call("scene.settings", {"Physics": {"Gravity": [0, 0, 0]}})
    rig = call("entity.create", {"name": "RagdollOverlay"})["id"]
    call("component.set", {"entity": rig, "component": "Animator", "data": {"Mesh": "Assets/Ragdoll.gltf", "PlayOnStart": False}})
    call("component.set", {"entity": rig, "component": "Ragdoll", "data": {"Enabled": True}})
    call("editor.camera", {"position": [0, 1, 5], "target": [0, 1, 0]})
    call("editor.select", {"entity": rig})
    call("editor.viewport", {"grid": False, "colliders": False, "lightGizmos": False, "cameraGizmos": False, "debugLines": False})
    call("play.start")
    call("lua.exec", {"code": "assert(Scene.FindEntityByName('RagdollOverlay'):IsRagdollActive())"})
    plain = Path(output, "ragdoll-plain.png")
    overlay = Path(output, "ragdoll-colliders.png")
    call("editor.screenshot", {"path": str(plain), "width": 640, "height": 480})
    call("editor.viewport", {"colliders": True})
    call("editor.screenshot", {"path": str(overlay), "width": 640, "height": 480})
    require(sum(a != b for a, b in zip(read_png(plain)[2], read_png(overlay)[2])) > 80,
            "Live ragdoll capsules did not appear with collider overlays enabled")
    call("lua.exec", {"code": "assert(Scene.FindEntityByName('RagdollOverlay'):RecoverRagdoll('Bend', 0))"})
    call("play.step", {"frames": 1, "assertNoErrors": True})
    call("lua.exec", {"code": "assert(not Scene.FindEntityByName('RagdollOverlay'):IsRagdollActive())"})
    call("play.stop")
    call("editor.viewport", {"colliders": False})
