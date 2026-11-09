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
import struct
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


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_dylibs(directory, architectures):
    """Empty but real dylibs named like the Vulkan loader and MoltenVK, for the given architectures: the release checks
    their architectures and signs them, while headless runs never load them."""
    directory.mkdir(parents=True)
    for name in ("libvulkan.1.dylib", "libMoltenVK.dylib"):
        flags = [flag for architecture in architectures for flag in ("-arch", architecture)]
        built = subprocess.run(["xcrun", "clang", "-dynamiclib", "-x", "c", "-", "-o", str(directory / name)] + flags, input="", capture_output=True, text=True)
        require(built.returncode == 0, f"cannot build a stand-in {name}: {built.stderr}")


def check_mach_o_parser(package_release):
    """mach_o_architectures on hand-made headers, on every platform."""
    with tempfile.TemporaryDirectory(prefix="BasaltMachO-") as temp:
        def architectures(data):
            path = Path(temp, "file")
            path.write_bytes(data)
            return package_release.mach_o_architectures(path)
        thin_arm64 = b"\xcf\xfa\xed\xfe" + struct.pack("<i", 0x0100000C) + bytes(24)
        require(architectures(thin_arm64) == ["arm64"], "a thin arm64 header")
        require(architectures(b"\xcf\xfa\xed\xfe" + struct.pack("<i", 0x01000007)) == ["x86_64"], "a thin x86_64 header")
        fat = b"\xca\xfe\xba\xbe" + struct.pack(">I", 2) + struct.pack(">iiIII", 0x01000007, 3, 4096, 1, 12) + struct.pack(">iiIII", 0x0100000C, 0, 8192, 1, 14)
        require(architectures(fat) == ["arm64", "x86_64"], "a universal header")
        fat64 = b"\xca\xfe\xba\xbf" + struct.pack(">I", 1) + struct.pack(">iiQQII", 0x0100000C, 0, 4096, 1, 14, 0)
        require(architectures(fat64) == ["arm64"], "a 64-bit universal header")
        require(architectures(b"\xca\xfe\xba\xbe" + struct.pack(">I", 1) + struct.pack(">iiIII", 18, 0, 4096, 1, 12)) == ["cpu-0x12"], "an unknown CPU type")
        # A Java class file (minor and major version where the slice count would be), a truncated table, text, nothing.
        for data in (b"\xca\xfe\xba\xbe\x00\x00\x00\x41", b"\xca\xfe\xba\xbe" + struct.pack(">I", 2) + bytes(20), b"stand-in", b"", fat[:7]):
            require(architectures(data) == [], f"not Mach-O: {data[:12]!r}")
    # The release name follows the CLI's architectures on macOS.
    if sys.platform == "darwin":
        with tempfile.TemporaryDirectory(prefix="BasaltMachO-") as temp:
            Path(temp, "basalt").write_bytes(fat)
            require(package_release.platform_name(Path(temp, "basalt")) == "macos-universal", "a universal CLI")
            Path(temp, "basalt").write_bytes(thin_arm64)
            require(package_release.platform_name(Path(temp, "basalt")) == "macos-arm64", "an arm64 CLI")


def main():
    cli = Path(sys.argv[1]).resolve()
    build = cli.parent.parent
    with_engine = "--with-engine" in sys.argv[2:]
    package_release = load("package_release", SCRIPT)
    version, platform = package_release.engine_version(), package_release.platform_name(cli)
    exe = package_release.EXE
    check_mach_o_parser(package_release)

    # A tag must name the engine version.
    require(run(["--check-tag", f"v{version}"]).returncode == 0, "the matching tag was refused")
    mismatch = run(["--check-tag", "v999.0.0"])
    require(mismatch.returncode == 1 and "does not match the engine version" in mismatch.stderr, f"a mismatched tag: {mismatch.stderr}")
    require(run(["--build", str(build), "--output", "x", "--notarize", "profile"]).returncode == 2, "--notarize without --sign-identity")
    # Each platform's signing options are refused elsewhere.
    foreign = [["--windows-certificate", "0" * 40]] if sys.platform != "win32" else []
    foreign += [["--sign-identity", "Developer ID Application: X"], ["--require-universal"]] if sys.platform != "darwin" else []
    for arguments in foreign:
        refused = run(["--build", str(build), "--output", "x"] + arguments)
        require(refused.returncode == 2 and ("is for" in refused.stderr or "are for" in refused.stderr), f"{arguments} on this platform: {refused.stderr}")
    bad_level = run(["--build", str(build), "--output", "x", "--gzip-level", "0"])
    require(bad_level.returncode == 2 and "--gzip-level" in bad_level.stderr and "invalid choice" in bad_level.stderr, f"--gzip-level 0: {bad_level.stderr}")

    # archive()'s Linux branch, here on every host but Windows (no symbolic links without privileges): the gzip level
    # reaches the header (XFL, byte 8: 4 for level 1, 2 for level 9), and the tarball keeps the folder's name, the
    # executable bits and an AppDir's AppRun link.
    if sys.platform != "win32":
        with tempfile.TemporaryDirectory(prefix="BasaltArchive-") as temp:
            folder = Path(temp, "App.AppDir")
            (folder / "usr").mkdir(parents=True)
            (folder / "usr" / "tool").write_text("#!/bin/sh\n")
            (folder / "usr" / "tool").chmod(0o755)
            (folder / "AppRun").symlink_to("usr/tool")
            for level, xfl in ((1, 4), (9, 2)):
                path = package_release.archive(folder, Path(temp, f"level{level}"), level, host="linux")
                header = path.read_bytes()[:10]
                require(path.name == f"level{level}.tar.gz" and header[8] == xfl, f"level {level}: {path.name}, header {header.hex()}")
                with tarfile.open(path) as file:
                    entries = {info.name: info for info in file.getmembers()}
                require(sorted(entries) == ["App.AppDir", "App.AppDir/AppRun", "App.AppDir/usr", "App.AppDir/usr/tool"], f"tarball members {sorted(entries)}")
                link, tool = entries["App.AppDir/AppRun"], entries["App.AppDir/usr/tool"]
                require(link.issym() and link.linkname == "usr/tool", "AppRun is not a link to usr/tool")
                require(tool.mode & 0o111 == 0o111, f"tool mode {tool.mode:o}")
    # Where archives are zips, the option is refused politely: a warning, then the run goes on (here to the unknown
    # sample).
    if package_release.writes_zip():
        ignored = run(["--build", str(build), "--output", "x", "--samples", "NoSuchSample", "--gzip-level", "1"])
        require(ignored.returncode == 1 and "--gzip-level has no effect here" in ignored.stdout, f"--gzip-level on a zip host: {ignored.stdout}{ignored.stderr}")
    unknown = run(["--build", str(build), "--output", "x", "--samples", "NoSuchSample"])
    require(unknown.returncode == 1 and "unknown samples: NoSuchSample" in unknown.stderr, f"an unknown sample: {unknown.stderr}")

    with tempfile.TemporaryDirectory(prefix="Basalt Release ") as temp:
        env = dict(os.environ)
        signtool_log = Path(temp, "signtool.jsonl")
        windows_signing = []
        if sys.platform == "darwin":
            architectures = package_release.mach_o_architectures(cli)
            # Libraries of another architecture than the tools' fail the release, naming the file.
            other = ["x86_64"] if architectures == ["arm64"] else ["arm64"]
            make_dylibs(Path(temp, "Other SDK", "lib"), other)
            mismatch = run(["--build", str(build), "--output", str(Path(temp, "mismatch")), "--samples", "Tetris"] + ([] if with_engine else ["--skip-engine"]),
                           dict(env, VULKAN_SDK=str(Path(temp, "Other SDK"))))
            require(mismatch.returncode == 1 and f"holds {', '.join(other)}, the tools {', '.join(architectures)}" in mismatch.stderr, f"mismatched libraries: {mismatch.stdout[-2000:]}{mismatch.stderr}")
            if architectures != package_release.UNIVERSAL:
                refused = run(["--build", str(build), "--output", str(Path(temp, "refused")), "--require-universal"], env)
                require(refused.returncode == 1 and "the build is not universal" in refused.stderr, f"--require-universal: {refused.stderr}")
            # Export bundles the Vulkan loader and MoltenVK; headless smoke tests never load them, so empty ones do.
            make_dylibs(Path(temp, "Vulkan SDK", "lib"), architectures)
            env["VULKAN_SDK"] = str(Path(temp, "Vulkan SDK"))
        elif sys.platform == "win32":
            # A stand-in signtool records what scripts/sign-windows.py asks of it.
            env.update(SIGNTOOL=str(load("WindowsSigning", Path(__file__).with_name("WindowsSigning.py")).make_stand_in(Path(temp))), STANDIN_LOG=str(signtool_log))
            windows_signing = ["--windows-certificate", "ab" * 20]
        output = Path(temp, "dist")
        # The fastest gzip level: level 9 takes minutes over a Debug build's binaries. Only Linux writes .tar.gz archives
        # (other hosts zip and would warn that the option does nothing).
        linux = sys.platform.startswith("linux")
        arguments = ["--build", str(build), "--output", str(output), "--samples", "Tetris"] + (["--gzip-level", "1"] if linux else []) + ([] if with_engine else ["--skip-engine"]) + windows_signing
        packaged = run(arguments, env)
        require(packaged.returncode == 0, f"package_release.py failed: {packaged.stdout}{packaged.stderr}")
        files = sorted(path.name for path in output.iterdir())
        checksums_name = f"SHA256SUMS-{platform}.txt"
        sample = next((name for name in files if name.startswith(f"Basalt-{version}-Tetris-{platform}")), None)
        engine = next((name for name in files if name.startswith(f"Basalt-{version}-{platform}.")), None)
        require(sample and checksums_name in files and bool(engine) == with_engine and len(files) == 2 + with_engine, f"unexpected release files {files}")

        # Checks the tarballs, which only Linux writes: the sample unless appimagetool built an AppImage, and always the
        # tools archive. Python's gzip records level 1 in the header (XFL, byte 8, is 4).
        archives = sorted(filter(None, [sample, engine]))
        tarballs = [name for name in archives if name.endswith(".tar.gz")]
        require(not linux or not engine or engine in tarballs, f"the tools archive is not a tarball: {engine}")
        for name in tarballs:
            with open(Path(output, name), "rb") as file:
                header = file.read(10)
            require(header[:2] == b"\x1f\x8b" and header[8] == 4, f"{name} was not compressed at level 1: header {header.hex()}")

        # Each archive's digest, computed here.
        expected = "".join(f"{hashlib.sha256(Path(output, name).read_bytes()).hexdigest()}  {name}\n" for name in archives)
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

        if sys.platform == "darwin" and with_engine:
            # The tools and their libraries are signed (ad hoc here), and the signatures survive the archive.
            extracted = Path(temp, "extracted")
            subprocess.run(["ditto", "-x", "-k", str(output / engine), str(extracted)], check=True)
            for name in ("BasaltEditor", "BasaltRuntime", "basalt", "libvulkan.1.dylib", "libMoltenVK.dylib"):
                verified = subprocess.run(["codesign", "--verify", "--strict", str(extracted / f"Basalt-{version}-{platform}" / "bin" / name)], capture_output=True, text=True)
                require(verified.returncode == 0, f"the packaged {name} is not signed: {verified.stderr}")
        if sys.platform == "win32":
            # The tools (with --with-engine) and the sample's executable, each signed and then verified.
            calls = [json.loads(line) for line in signtool_log.read_text().splitlines()]
            signed = sorted(Path(argument).name for call in calls if call[0] == "sign" for argument in call if argument.endswith(".exe"))
            expected = sorted(["Tetris.exe"] + (["basalt.exe", "BasaltEditor.exe", "BasaltRuntime.exe"] if with_engine else []))
            require(signed == expected and all("/sha1" in call and "ab" * 20 in call for call in calls if call[0] == "sign"), f"signtool calls: {calls}")
            require(["/d", "Tetris"] == next(call[call.index("/d"):call.index("/d") + 2] for call in calls if "/d" in call), f"the sample's description: {calls}")
            require(sum(call[0] == "verify" for call in calls) == 1 + with_engine, f"verifications: {calls}")

        # The output must be empty: archives are never mixed with an earlier run's.
        again = run(arguments, env)
        require(again.returncode == 1 and "is not empty" in again.stderr, f"a second run into the same output: {again.stderr}")
    print("ReleasePackaging: OK")


if __name__ == "__main__":
    main()
