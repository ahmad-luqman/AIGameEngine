#!/usr/bin/env python3
"""Runs scripts/package_release.py the way the release workflow does, on one sample.

Usage: ReleasePackaging.py <basalt CLI> [--with-engine]   (--with-engine: also the tools archive; needs BasaltEditor)
"""
import hashlib
import importlib.util
import json
import os
import posixpath
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "package_release.py"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(arguments, env=None):
    return subprocess.run([sys.executable, "-B", str(SCRIPT)] + arguments, capture_output=True, text=True, env=env, timeout=900)


def members(archive):
    """Member names of a zip or tar.gz archive, with which ones are symbolic links."""
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as file:
            return {info.filename.rstrip("/"): False for info in file.infolist()}
    if archive.name.endswith(".tar.gz"):
        with tarfile.open(archive) as file:
            return {info.name: info.issym() for info in file.getmembers()}
    return {}


def main():
    build = Path(sys.argv[1]).resolve().parent.parent
    with_engine = "--with-engine" in sys.argv[2:]
    spec = importlib.util.spec_from_file_location("package_release", SCRIPT)
    package_release = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(package_release)
    version, platform = package_release.engine_version(), package_release.platform_name()
    exe = package_release.EXE

    # A tag must name the engine version.
    require(run(["--check-tag", f"v{version}"]).returncode == 0, "the matching tag was refused")
    mismatch = run(["--check-tag", "v999.0.0"])
    require(mismatch.returncode == 1 and "does not match the engine version" in mismatch.stderr, f"a mismatched tag: {mismatch.stderr}")
    require(run(["--build", str(build), "--output", "x", "--notarize", "profile"]).returncode == 2, "--notarize without --sign-identity")
    require(run(["--build", str(build), "--output", "x", "--compression-level", "0"]).returncode == 2, "--compression-level 0")
    unknown = run(["--build", str(build), "--output", "x", "--samples", "NoSuchSample"])
    require(unknown.returncode == 1 and "unknown samples: NoSuchSample" in unknown.stderr, f"an unknown sample: {unknown.stderr}")

    with tempfile.TemporaryDirectory(prefix="Basalt Release ") as temp:
        env = dict(os.environ)
        if sys.platform == "darwin":
            # Export bundles the Vulkan loader and MoltenVK; headless smoke tests never load them, so stand-ins do.
            sdk = Path(temp, "Vulkan SDK", "lib")
            sdk.mkdir(parents=True)
            for name in ("libvulkan.1.dylib", "libMoltenVK.dylib"):
                (sdk / name).write_text("stand-in")
            env["VULKAN_SDK"] = str(sdk.parent)
        output = Path(temp, "dist")
        # The fastest gzip level: level 9 takes minutes over a Debug build's binaries (Linux; other hosts zip).
        arguments = ["--build", str(build), "--output", str(output), "--samples", "Tetris", "--compression-level", "1"] + ([] if with_engine else ["--skip-engine"])
        packaged = run(arguments, env)
        require(packaged.returncode == 0, f"package_release.py failed: {packaged.stdout}{packaged.stderr}")
        files = sorted(path.name for path in output.iterdir())
        checksums_name = f"SHA256SUMS-{platform}.txt"
        sample = next((name for name in files if name.startswith(f"Basalt-{version}-Tetris-{platform}")), None)
        engine = next((name for name in files if name.startswith(f"Basalt-{version}-{platform}.")), None)
        require(sample and checksums_name in files and bool(engine) == with_engine and len(files) == 2 + with_engine, f"unexpected release files {files}")

        # gzip records level 1 as "fastest" in its header (XFL, byte 8; level 9 records 2).
        for name in files:
            if name.endswith(".tar.gz"):
                with open(Path(output, name), "rb") as file:
                    header = file.read(10)
                require(header[:2] == b"\x1f\x8b" and header[8] == 4, f"{name} was not compressed at level 1: header {header.hex()}")

        # Each archive's digest, computed here.
        expected = "".join(f"{hashlib.sha256(Path(output, name).read_bytes()).hexdigest()}  {name}\n" for name in sorted(filter(None, [sample, engine])))
        checksums = Path(output, checksums_name).read_text()
        require(checksums == expected, f"checksums: {checksums!r}, expected {expected!r}")

        # The sample archive holds the platform package, whose Assets are packed into Assets.bpak.
        contents = members(output / sample)
        if sys.platform == "darwin":
            require({"Tetris.app/Contents/Info.plist", "Tetris.app/Contents/MacOS/Tetris", "Tetris.app/Contents/Resources/Assets.bpak"} <= contents.keys(), f"sample archive: {sorted(contents)[:20]}")
        elif sys.platform == "win32":
            require({"Tetris/Tetris.exe", "Tetris/Project.bproject", "Tetris/Assets.bpak"} <= contents.keys(), f"sample archive: {sorted(contents)[:20]}")
        elif sample.endswith(".tar.gz"):
            require(contents.get("Tetris.AppDir/AppRun") is True and {"Tetris.AppDir/Tetris", "Tetris.AppDir/Assets.bpak"} <= contents.keys(), f"sample archive: {sorted(contents)[:20]}")
        else:
            require(sample.endswith(".AppImage"), f"sample: {sample}")
        require(not any("/Assets/" in name for name in contents), f"the sample ships loose assets: {[name for name in contents if '/Assets/' in name][:5]}")

        if with_engine:
            folder = f"Basalt-{version}-{platform}"
            contents = members(output / engine)
            wanted = {f"{folder}/bin/{tool}{exe}" for tool in ("BasaltEditor", "BasaltRuntime", "basalt")}
            wanted |= {f"{folder}/Samples/Tetris/Project.bproject", f"{folder}/Docs/Shipping.md", f"{folder}/LICENSE"}
            if sys.platform == "darwin":
                wanted |= {f"{folder}/bin/libvulkan.1.dylib", f"{folder}/bin/libMoltenVK.dylib", f"{folder}/bin/vulkan/icd.d/MoltenVK_icd.json"}
            require(wanted <= contents.keys(), f"tools archive lacks {sorted(wanted - contents.keys())}")
            if sys.platform == "darwin":
                # The manifest must reach the MoltenVK next to the tools.
                with zipfile.ZipFile(output / engine) as archive:
                    library = json.loads(archive.read(f"{folder}/bin/vulkan/icd.d/MoltenVK_icd.json"))["ICD"]["library_path"]
                require(posixpath.normpath(f"{folder}/bin/vulkan/icd.d/{library}") == f"{folder}/bin/libMoltenVK.dylib", f"the tools' manifest names {library}")

        # The output must be empty: archives are never mixed with an earlier run's.
        again = run(arguments, env)
        require(again.returncode == 1 and "is not empty" in again.stderr, f"a second run into the same output: {again.stderr}")
    print("ReleasePackaging: OK")


if __name__ == "__main__":
    main()
