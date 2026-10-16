"""Project Settings window and editor.panel checks through the editor's public commands."""
from pathlib import Path

from LocalShadows import read_png


def check_project_settings(call, require, output):
    opened = call("editor.panel", {"name": "Project Settings", "open": True, "focus": True})
    require(opened == {"name": "Project Settings", "open": True}, f"Project Settings did not open: {opened}")
    call("editor.panel", {"name": "Viewport", "open": False}, error="cannot be closed")
    call("editor.panel", {"name": "Timeline"}, error="name")

    layers = {"Names": ["Default", "Player", "Debris"], "IgnoredPairs": [["Debris", "Player"], ["Debris", "Debris"]]}
    call("project.set", {"randomSeed": 1234, "windowWidth": 800, "windowHeight": 600, "fixedTimestep": 0.02, "physicsLayers": layers})
    info = call("project.info")
    require(info["randomSeed"] == 1234 and info["windowWidth"] == 800 and info["windowHeight"] == 600, f"project.set did not apply: {info}")
    require(abs(info["fixedTimestep"] - 0.02) < 1e-7, f"fixed timestep: {info}")
    require(info["physicsLayers"]["Names"] == layers["Names"] and len(info["physicsLayers"]["IgnoredPairs"]) == 2, f"layers: {info}")

    # The open scene can keep its own step; the window shows it and offers to remove it.
    call("scene.settings", {"Physics": {"FixedTimestep": 0.01}})
    scene = call("scene.info")
    require(abs(scene["fixedTimestepOverride"] - 0.01) < 1e-7 and abs(scene["fixedTimestep"] - 0.01) < 1e-7, f"scene override: {scene}")
    call("scene.settings", {"Physics": {"FixedTimestep": None}})
    require(abs(call("scene.info")["fixedTimestep"] - 0.02) < 1e-7, "clearing the override did not restore the project step")

    report = call("project.validate")
    require(not report["valid"] and "no StartScene" in report["issues"][0]["message"], f"a project without a start scene validated: {report}")
    call("scene.save", {"path": "Assets/Scenes/Settings.bscene"})
    call("project.set", {"startScene": "Assets/Scenes/Settings.bscene"})
    report = call("project.validate")
    require(report["valid"], f"the saved scene should validate: {report}")

    # The window draws every section (layers, matrix, validation) without asserting or logging errors.
    path = Path(output, "project-settings.png")
    call("editor.window_screenshot", {"path": str(path)})
    call("editor.viewport")
    width, height, pixels = read_png(path)
    require(width > 0 and height > 0 and len(set(pixels[::97])) > 8, "the window screenshot is blank")

    closed = call("editor.panel", {"name": "Project Settings", "open": False})
    require(closed["open"] is False, f"Project Settings did not close: {closed}")
    call("editor.panel", {"name": "Project Settings", "focus": True}, error="open the Project Settings window")
