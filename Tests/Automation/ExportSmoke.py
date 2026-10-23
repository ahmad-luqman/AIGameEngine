#!/usr/bin/env python3
"""Validate, export and smoke-test a project through the basalt CLI, as a release pipeline would (no GPU)."""
import json
import os
from pathlib import Path
import platform
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

SCENE = {
    "Format": "BasaltScene",
    "Version": 1,
    "Name": "Main",
    "Entities": [
        {"ID": 1, "Name": "Camera", "Components": {
            "Transform": {"Translation": [0, 1, 5]}, "Camera": {"FOV": 50, "Near": 0.1, "Far": 100}}},
        {"ID": 2, "Name": "Crate", "Components": {
            "Mesh": {"Mesh": "Assets/Models/Crate.glb"},
            "Script": {"Script": "Assets/Scripts/Spin.lua"}}},
    ],
}
# Saves on start, so the test can check where the smoke run's save data went.
GOOD_SCRIPT = "local Spin = {}\nfunction Spin:OnCreate()\n\tStorage.Set('Launched', true)\n\tassert(Storage.Save())\nend\nfunction Spin:OnUpdate(dt)\n\tself.Entity.Rotation = Quat.AngleAxis(dt, Vec3(0, 1, 0)) * self.Entity.Rotation\nend\nreturn Spin\n"
HANGING_SCRIPT = "local Spin = {}\nfunction Spin:OnUpdate(dt)\n\twhile true do end\nend\nreturn Spin\n"
BROKEN_SCRIPT = "local Spin = {}\nfunction Spin:OnUpdate(dt)\n\terror('spin failed')\nend\nreturn Spin\n"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def write_png(path, width, height):
    """An opaque orange RGBA PNG."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    rows = b"".join(b"\x00" + bytes([230, 120, 40, 255]) * width for _ in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def png_size(path):
    """Width and height from a PNG file's IHDR chunk."""
    data = Path(path).read_bytes()
    require(data.startswith(b"\x89PNG\r\n\x1a\n") and data[12:16] == b"IHDR", f"{path} is not a PNG")
    return struct.unpack(">II", data[16:24])


def check_manifest(manifest, library):
    """A MoltenVK ICD manifest whose library path, relative to the manifest, reaches the bundled library."""
    path = json.loads(Path(manifest).read_text())["ICD"]["library_path"]
    require((Path(manifest).parent / path).resolve() == Path(library).resolve(), f"{manifest} names {path}, not {library}")


def check_windows_resources(executable, has_icon):
    """The exported executable's version resource, read by Windows itself, and its icon group (if any)."""
    import ctypes
    from ctypes import wintypes
    version = ctypes.WinDLL("version")
    version.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    version.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    version.VerQueryValueW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
    size = version.GetFileVersionInfoSizeW(str(executable), None)
    require(size > 0, f"{executable} has no version resource")
    data = ctypes.create_string_buffer(size)
    require(version.GetFileVersionInfoW(str(executable), 0, size, data), "GetFileVersionInfoW failed")

    def query(path):
        pointer, length = ctypes.c_void_p(), wintypes.UINT()
        require(version.VerQueryValueW(data, path, ctypes.byref(pointer), ctypes.byref(length)), f"no {path} in the version resource")
        return pointer, length.value

    pointer, _ = query("\\")
    fixed = struct.unpack("<13I", ctypes.string_at(pointer, 52))
    require(fixed[0] == 0xFEEF04BD and fixed[2] == (2 << 16 | 5) and fixed[3] == (0 << 16 | 7), f"fixed file info {fixed}")
    strings = {}
    for key in ("CompanyName", "ProductName", "ProductVersion", "OriginalFilename"):
        pointer, length = query(f"\\StringFileInfo\\040904B0\\{key}")
        strings[key] = ctypes.wstring_at(pointer, length).rstrip("\0")
    require(strings == {"CompanyName": "Smoke Studio", "ProductName": "Smoke Game", "ProductVersion": "2.5.0.7", "OriginalFilename": "Smoke_Game.exe"}, f"version strings {strings}")

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.LoadLibraryExW.restype = wintypes.HMODULE
    kernel32.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
    kernel32.FindResourceW.restype = ctypes.c_void_p
    kernel32.FindResourceW.argtypes = [wintypes.HMODULE, wintypes.LPCWSTR, ctypes.c_void_p]
    kernel32.FreeLibrary.argtypes = [wintypes.HMODULE]
    module = kernel32.LoadLibraryExW(str(executable), None, 0x2 | 0x20)  # as a data file and image resource
    require(module, f"cannot open {executable} as a data file")
    try:
        found = bool(kernel32.FindResourceW(module, "GLFW_ICON", 14))
        require(found == has_icon, f"GLFW_ICON icon group (RT_GROUP_ICON) present: {found}, expected {has_icon}")
    finally:
        kernel32.FreeLibrary(module)


def check_package(exported, output, has_icon=True):
    """The host platform's package layout around the exported executable."""
    executable, package = Path(exported["executable"]), Path(exported["package"])
    warnings = exported.get("warnings", [])
    # Every platform says when the package has no project icon.
    require(has_icon != any("the project has no icon" in warning for warning in warnings), f"no-icon warning: {warnings}")
    if sys.platform == "darwin":
        require(package == output / "Smoke_Game.app" and executable == package / "Contents" / "MacOS" / "Smoke_Game", f"unexpected bundle {exported}")
        contents = package / "Contents"
        info = plistlib.loads((contents / "Info.plist").read_bytes())
        expected = {"CFBundleExecutable": "Smoke_Game", "CFBundleIdentifier": "com.Smoke-Studio.Smoke-Game", "CFBundleShortVersionString": "2.5.0",
                    "CFBundleVersion": "2.5.0", "CFBundlePackageType": "APPL", "CFBundleIconFile": "Smoke_Game.icns" if has_icon else None}
        require(all(info.get(key) == value for key, value in expected.items()), f"Info.plist: {info}")
        require((contents / "Resources" / "Smoke_Game.icns").is_file() == has_icon and len(warnings) == (0 if has_icon else 1), f"icon file or warnings: {exported}")
        for relative in ("Resources/Project.bproject", "Resources/Assets/Scenes/Main.bscene", "Resources/vulkan/icd.d/MoltenVK_icd.json",
                         "Frameworks/libvulkan.1.dylib", "Frameworks/libMoltenVK.dylib", "PkgInfo"):
            require((contents / relative).is_file(), f"the bundle has no Contents/{relative}")
        # The manifest must reach MoltenVK in Frameworks from Resources (VulkanLoader finds the loader in ../Frameworks).
        check_manifest(contents / "Resources" / "vulkan" / "icd.d" / "MoltenVK_icd.json", contents / "Frameworks" / "libMoltenVK.dylib")
        if not has_icon:
            return
        if shutil.which("iconutil"):
            with tempfile.TemporaryDirectory() as iconset:
                converted = subprocess.run(["iconutil", "-c", "iconset", str(contents / "Resources" / "Smoke_Game.icns"), "-o", str(Path(iconset, "x.iconset"))], capture_output=True, text=True)
                require(converted.returncode == 0, f"iconutil rejects the icon: {converted.stderr}")
                require((Path(iconset, "x.iconset", "icon_256x256.png")).is_file(), "the icon has no 256 px image")
        # scripts/sign-macos.sh signs ad hoc without a Developer ID; the signed bundle verifies and still runs.
        sign = Path(__file__).resolve().parents[2] / "scripts" / "sign-macos.sh"
        signed = subprocess.run(["bash", str(sign), str(package)], capture_output=True, text=True)
        require(signed.returncode == 0 and "(ad hoc)" in signed.stdout, f"sign-macos.sh: {signed.stdout}{signed.stderr}")
        refused = subprocess.run(["bash", str(sign), str(package), "--notarize", "profile"], capture_output=True, text=True)
        require(refused.returncode == 2 and "--notarize needs --identity" in refused.stderr, f"sign-macos.sh --notarize without identity: {refused.stderr}")
    elif sys.platform == "win32":
        require(package == output and executable == output / "Smoke_Game.exe" and len(warnings) == (0 if has_icon else 1), f"unexpected package {exported}")
        check_windows_resources(executable, has_icon)
    else:
        app_dir = output / "Smoke_Game.AppDir"
        require(executable == app_dir / "Smoke_Game", f"unexpected AppDir {exported}")
        require(os.readlink(app_dir / "AppRun") == "Smoke_Game" and os.readlink(app_dir / ".DirIcon") == "Smoke_Game.png", "AppRun/.DirIcon links")
        desktop = (app_dir / "Smoke_Game.desktop").read_text()
        require("Name=Smoke Game\n" in desktop and "Exec=Smoke_Game\n" in desktop and "X-AppImage-Version=2.5.0.7\n" in desktop, f"desktop entry: {desktop}")
        require((app_dir / "Project.bproject").is_file(), "AppDir contents")
        # The project icon padded to a square, or Basalt's default icon (with a warning).
        require(png_size(app_dir / "Smoke_Game.png") == ((300, 300) if has_icon else (256, 256)), f"AppDir icon size {png_size(app_dir / 'Smoke_Game.png')}")
        require(has_icon or any("uses Basalt's default icon" in warning for warning in warnings), f"default icon warning: {warnings}")
        missing_tool = any("appimagetool was not found" in warning for warning in warnings)
        if missing_tool:
            require(package == app_dir and not os.environ.get("APPIMAGETOOL"), f"no AppImage although APPIMAGETOOL is set: {exported}")
        else:
            architecture = "aarch64" if platform.machine().lower() in ("aarch64", "arm64") else "x86_64"
            require(package == output / f"Smoke_Game-2.5.0.7-{architecture}.AppImage" and package.is_file(), f"no AppImage: {exported}")


def main():
    cli, model = map(lambda value: str(Path(value).resolve()), sys.argv[1:3])
    # Spaces in every path: the smoke test must quote the exported executable and run it from its own folder.
    with tempfile.TemporaryDirectory(prefix="Basalt Export ") as temp:
        env = dict(os.environ, BASALT_USER_DATA_DIR=str(Path(temp, "User Data")))
        if sys.platform == "darwin":
            # Export bundles the Vulkan loader and MoltenVK; headless runs never load them, so stand-ins do.
            sdk = Path(temp, "Vulkan SDK", "lib")
            sdk.mkdir(parents=True)
            for name in ("libvulkan.1.dylib", "libMoltenVK.dylib"):
                (sdk / name).write_text("stand-in")
            env["VULKAN_SDK"] = str(sdk.parent)
        project = Path(temp, "Smoke Game")

        def run(command, params=None, project_dir=project):
            arguments = [cli] + (["--project", str(project_dir)] if project_dir else []) + [command, json.dumps(params or {})]
            completed = subprocess.run(arguments, capture_output=True, text=True, timeout=300, env=env)
            start = completed.stdout.find("{")
            require(start >= 0, f"{command}: no JSON in output: {completed.stdout}{completed.stderr}")
            response = json.loads(completed.stdout[start:])
            require((completed.returncode == 0) == response.get("ok"), f"{command}: exit code {completed.returncode} disagrees with {response}")
            return response

        def ok(command, params=None, project_dir=project):
            response = run(command, params, project_dir)
            require(response.get("ok"), f"{command}: {response}")
            return response["result"]

        def fails(command, params, expected):
            response = run(command, params)
            require(not response.get("ok") and expected in response.get("error", ""), f"{command}: expected an error containing {expected!r}, got {response}")
            return response["error"]

        ok("project.create", {"path": str(project), "name": "Smoke Game"}, None)
        shutil.copy(model, project / "Assets" / "Models" / "Crate.glb")
        (project / "Assets" / "Scripts" / "Spin.lua").write_text(GOOD_SCRIPT)
        (project / "Assets" / "Scenes" / "Main.bscene").write_text(json.dumps(SCENE))
        ok("project.set", {"startScene": "Assets/Scenes/Main.bscene"})

        report = ok("project.validate")
        require(report["valid"] and report["errors"] == 0 and report["warnings"] == 0, f"clean project reported {report}")

        output = Path(temp, "Exported Build")
        exported = ok("project.export", {"output": str(output), "smokeTestFrames": 30})
        executable = Path(exported["executable"])
        require(executable.parent == output and executable.stem == "Smoke_Game", f"unexpected executable {executable}")
        smoke = exported["smokeTest"]
        require(smoke["passed"] and smoke["outcome"] == "exited" and smoke["exitCode"] == 0, f"smoke test: {smoke}")
        require("Runtime: playing 'Assets/Scenes/Main.bscene'" in smoke["log"], f"smoke log: {smoke['log']}")
        require(exported["validation"]["valid"], "export did not report its validation")
        # The game saved on start, and not into the user's data directory.
        require(not Path(temp, "User Data", "Smoke Game").exists(), "the smoke test wrote save data to the user directory")

        rerun = ok("project.smoke_test", {"executable": str(executable), "frames": 5, "assertPassed": True})
        require(rerun["passed"], f"standalone smoke test: {rerun}")

        # A script that fails at runtime still compiles, so validation passes and only the smoke test catches it.
        (project / "Assets" / "Scripts" / "Spin.lua").write_text(BROKEN_SCRIPT)
        require(ok("project.validate")["valid"], "a runtime script error should not fail validation")
        error = fails("project.export", {"output": str(output), "smokeTestFrames": 10}, "exited with code 7: scripts reported errors")
        require("spin failed" in error, f"the failure does not show the script error: {error}")
        failed = ok("project.smoke_test", {"executable": str(executable), "frames": 10})
        require(not failed["passed"] and failed["exitCode"] == 7 and "spin failed" in failed["log"], f"smoke test of a broken build: {failed}")
        fails("project.smoke_test", {"executable": str(executable), "frames": 10, "assertPassed": True}, "smoke test failed")

        # A game that hangs is stopped at the timeout and reported as such.
        (project / "Assets" / "Scripts" / "Spin.lua").write_text(HANGING_SCRIPT)
        ok("project.export", {"output": str(output)})
        if sys.platform == "darwin":
            # A folder export's manifest reaches the MoltenVK next to the executable.
            check_manifest(output / "vulkan" / "icd.d" / "MoltenVK_icd.json", output / "libMoltenVK.dylib")
        hung = ok("project.smoke_test", {"executable": str(executable), "frames": 10, "timeout": 1})
        require(not hung["passed"] and hung["outcome"] == "timedOut" and hung["exitCode"] is None and "was stopped" in hung["summary"], f"a hanging game: {hung}")

        # The platform package: icon, version and company reach the bundle, version resource or desktop entry, and the
        # game still runs from inside it.
        (project / "Assets" / "Scripts" / "Spin.lua").write_text(GOOD_SCRIPT)
        write_png(project / "Assets" / "Textures" / "Icon.png", 300, 200)
        ok("project.set", {"version": "2.5.0.7", "company": "Smoke Studio", "icon": "Assets/Textures/Icon.png"})
        require(ok("project.validate")["warnings"] == 0, "a 300 x 200 icon is large enough")
        # Non-ASCII where the platform's CLI arguments can carry it (the Windows CLI reads narrow arguments).
        package_output = Path(temp, "Packaged Build" if sys.platform == "win32" else "Packaged Build \u00e9")
        packaged = ok("project.export", {"output": str(package_output), "package": True, "smokeTestFrames": 10})
        require(packaged["smokeTest"]["passed"], f"packaged smoke test: {packaged['smokeTest']}")
        check_package(packaged, package_output)
        rerun = ok("project.smoke_test", {"executable": packaged["executable"], "frames": 5, "assertPassed": True})
        require(rerun["passed"], f"the package does not run after checking it: {rerun}")
        # Exporting again replaces the package (the output is recognized as a previous export), leaving nothing stale.
        project_file = Path(packaged["executable"]).parent / "Project.bproject"
        if sys.platform == "darwin":
            project_file = Path(packaged["package"]) / "Contents" / "Resources" / "Project.bproject"
        stale = project_file.parent / "Assets" / "Stale.txt"
        stale.write_text("from an earlier export")
        again = ok("project.export", {"output": str(package_output), "package": True})
        require(again["package"] == packaged["package"] and not stale.exists(), f"re-export moved the package or kept a stale file: {again}")
        fails("project.export", {"output": str(package_output), "appImageTool": "appimagetool"}, "appImageTool needs package: true")
        # An icon that no longer decodes stops the export before it touches the previous package.
        icon_bytes = (project / "Assets" / "Textures" / "Icon.png").read_bytes()
        (project / "Assets" / "Textures" / "Icon.png").write_bytes(b"\x89PNG\r\n\x1a\nbroken")
        fails("project.export", {"output": str(package_output), "package": True, "validate": False}, "cannot decode icon")
        require(project_file.is_file(), "a refused export removed the previous package")
        (project / "Assets" / "Textures" / "Icon.png").write_bytes(icon_bytes)
        # Without an icon: no icon file or resource, and Basalt's icon in an AppImage.
        ok("project.set", {"icon": ""})
        plain = ok("project.export", {"output": str(package_output), "package": True, "smokeTestFrames": 5})
        check_package(plain, package_output, has_icon=False)

        # A missing asset is caught before anything is exported.
        (project / "Assets" / "Scripts" / "Spin.lua").write_text(GOOD_SCRIPT)
        (project / "Assets" / "Models" / "Crate.glb").unlink()
        report = ok("project.validate")
        require(not report["valid"] and any("Assets/Models/Crate.glb" in issue["message"] for issue in report["issues"]), f"missing model: {report}")
        before = executable.stat().st_mtime_ns
        fails("project.export", {"output": str(output), "smokeTestFrames": 10}, "not exported: project validation found 1 error")
        require(executable.stat().st_mtime_ns == before, "a refused export modified the previous build")
    print("ExportSmoke: OK")


if __name__ == "__main__":
    main()
