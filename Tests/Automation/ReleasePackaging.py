#!/usr/bin/env python3
"""Runs scripts/package_release.py the way the release workflow does, on one sample.

Usage: ReleasePackaging.py <breccia CLI> [--with-engine]   (--with-engine: also the tools archive; needs BrecciaEditor)
"""
import hashlib
import importlib.util
import json
import os
import posixpath
from pathlib import Path
import shutil
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


def make_dylibs(directory, architectures, molten_vk_architectures=None):
    """Empty but real dylibs named like the Vulkan loader and MoltenVK, for the given architectures (MoltenVK's may
    differ): the release checks their architectures and signs them, while headless runs never load them."""
    directory.mkdir(parents=True)
    for name in ("libvulkan.1.dylib", "libMoltenVK.dylib"):
        wanted = molten_vk_architectures if name == "libMoltenVK.dylib" and molten_vk_architectures else architectures
        flags = [flag for architecture in wanted for flag in ("-arch", architecture)]
        built = subprocess.run(["xcrun", "clang", "-dynamiclib", "-x", "c", "-", "-o", str(directory / name)] + flags, input="", capture_output=True, text=True)
        require(built.returncode == 0, f"cannot build a stand-in {name}: {built.stderr}")


def check_sign_macos_folder():
    """scripts/sign-macos.sh on a folder: it signs the libraries and Mach-O executables directly in it, leaves links,
    scripts and subfolders alone, and stops at Mach-O code it cannot place."""
    sign = ROOT / "scripts" / "sign-macos.sh"
    with tempfile.TemporaryDirectory(prefix="Breccia Sign ") as temp:
        folder = Path(temp, "Game")
        folder.mkdir()
        (folder / "sub").mkdir()
        for path, flags in ((folder / "Game", []), (folder / "sub" / "Inner", []), (folder / "libThing.dylib", ["-dynamiclib"])):
            built = subprocess.run(["xcrun", "clang", "-x", "c", "-", "-o", str(path)] + flags, input="int main(void) { return 0; }\n", capture_output=True, text=True)
            require(built.returncode == 0, f"cannot build {path.name}: {built.stderr}")
        (folder / "run.sh").write_text("#!/bin/sh\n")
        (folder / "run.sh").chmod(0o755)
        (folder / "Link").symlink_to("Game")

        def signature(path):
            return subprocess.run(["codesign", "-dv", str(path)], capture_output=True, text=True).stderr

        # Mach-O code that is neither a .dylib nor executable stops the script.
        shutil.copy2(folder / "Game", folder / "Stray")
        (folder / "Stray").chmod(0o644)
        refused = subprocess.run(["bash", str(sign), str(folder)], capture_output=True, text=True)
        require(refused.returncode == 1 and "Stray' is Mach-O code that is neither a .dylib nor executable" in refused.stderr, f"a stray Mach-O file: {refused.stderr}")
        (folder / "Stray").unlink()
        signed = subprocess.run(["bash", str(sign), str(folder)], capture_output=True, text=True)
        require(signed.returncode == 0 and "signed 2 files" in signed.stdout, f"sign-macos.sh on a folder: {signed.stdout}{signed.stderr}")
        # The linker's own signature says linker-signed; the script's ad hoc one does not.
        for name in ("Game", "libThing.dylib"):
            require("flags=0x2(adhoc)" in signature(folder / name), f"{name} was not signed: {signature(folder / name)}")
        require("linker-signed" in signature(folder / "sub" / "Inner"), "a subfolder's executable was signed")
        require("not signed at all" in signature(folder / "run.sh"), "a script was signed")
        # A file it cannot read stops it before anything is signed, a library (which it takes by name) included.
        for secret in ("Secret", "libSecret.dylib"):
            (folder / secret).write_text("unreadable")
            (folder / secret).chmod(0)
            refused = subprocess.run(["bash", str(sign), str(folder)], capture_output=True, text=True)
            (folder / secret).unlink()
            require(refused.returncode == 1 and f"cannot read '{folder / secret}'" in refused.stderr, f"an unreadable {secret}: {refused.stderr}")
        empty = Path(temp, "Empty")
        empty.mkdir()
        refused = subprocess.run(["bash", str(sign), str(empty)], capture_output=True, text=True)
        require(refused.returncode == 1 and "holds no libraries or executables" in refused.stderr, f"an empty folder: {refused.stderr}")


def call_main(package_release, arguments):
    """package_release.main() in this process, with its exit code and output."""
    import contextlib
    import io
    out, err = io.StringIO(), io.StringIO()
    saved = sys.argv
    sys.argv = ["package_release.py"] + arguments
    code = 0
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            package_release.main()
    except SystemExit as error:
        code = error.code
    finally:
        sys.argv = saved
    return code, out.getvalue(), err.getvalue()


def patched(module, **replacements):
    """Replaces module attributes for a with block."""
    import contextlib

    @contextlib.contextmanager
    def scope():
        saved = {name: getattr(module, name) for name in replacements}
        for name, value in replacements.items():
            setattr(module, name, value)
        try:
            yield
        finally:
            for name, value in saved.items():
                setattr(module, name, value)
    return scope()


def check_decisions(package_release, build):
    """package_release's decisions in this process, with its probes and the signing scripts stood in for."""
    import argparse
    import contextlib
    import io
    import types

    def exits(function, *arguments):
        err = io.StringIO()
        try:
            with contextlib.redirect_stderr(err):
                function(*arguments)
        except SystemExit as error:
            return error.code, err.getvalue()
        return 0, err.getvalue()

    with tempfile.TemporaryDirectory(prefix="BrecciaDecisions-") as temp:
        # check_architectures wants exactly the tools' architectures: extra ones and missing files fail too.
        universal = Path(temp, "universal")
        universal.write_bytes(b"\xca\xfe\xba\xbe" + struct.pack(">I", 2) + struct.pack(">iiIII", 0x01000007, 3, 4096, 1, 12) + struct.pack(">iiIII", 0x0100000C, 0, 8192, 1, 14))
        code, err = exits(package_release.check_architectures, [universal], ("arm64",), "the tools")
        require(code == 1 and "holds arm64, x86_64, the tools arm64: it holds architectures the tools do not" in err, f"extra architectures: {err}")
        code, err = exits(package_release.check_architectures, [Path(temp, "missing")], ("arm64",), "the tools")
        require(code == 1 and "missing is missing" in err, f"a missing library: {err}")

        # rosetta_problem names why x86-64 code cannot run.
        failed = types.SimpleNamespace(returncode=1, stderr="Bad CPU type in executable\n")
        with patched(package_release.subprocess, run=lambda *a, **k: failed):
            problem = package_release.rosetta_problem()
        require(problem and "exited with code 1" in problem and "Bad CPU type" in problem, f"rosetta_problem on a failed probe: {problem}")

        def missing_arch(*a, **k):
            raise OSError("no such file: arch")
        with patched(package_release.subprocess, run=missing_arch):
            problem = package_release.rosetta_problem()
        require(problem and "could not run" in problem and "no such file" in problem, f"rosetta_problem without arch: {problem}")

        # A signing script that fails stops the release with its exit code, as does one that cannot start.
        with patched(package_release.subprocess, run=lambda *a, **k: types.SimpleNamespace(returncode=3)):
            code, err = exits(package_release.run_signing, ["sign"], "'X.app'")
        require(code == 1 and "signing 'X.app' failed (exit code 3" in err, f"a failed signing: {err}")
        with patched(package_release.subprocess, run=missing_arch):
            code, err = exits(package_release.run_signing, ["sign"], "'X.app'")
        require(code == 1 and "signing 'X.app' could not start" in err, f"a signing script that cannot start: {err}")

        # Windows signing passes the target's certificate arguments, a signing service's included.
        commands = []
        target = package_release.Target(platform="windows-x64", architectures=(), run_x86_64=False, check_sample="Tetris",
                                        windows_signing=("--dlib", "D.dll", "--dlib-metadata", "m.json", "--timestamp-url", "http://t"))
        with patched(package_release, run_signing=lambda command, what: commands.append(command)):
            package_release.sign_windows([Path("Export")], target, "Tetris")
        require(commands and commands[0][2:] == ["Export", "--dlib", "D.dll", "--dlib-metadata", "m.json", "--timestamp-url", "http://t", "--description", "Tetris"], f"sign_windows: {commands}")
        try:
            package_release.Target(platform="macos-arm64", architectures=("arm64",), run_x86_64=True, check_sample="Tetris")
            require(False, "a thin build was given an x86-64 half")
        except ValueError:
            pass

        # On macOS: a universal CLI on a Mac that cannot run x86-64 code fails with --require-universal and only warns
        # without it; a CLI that is no Mach-O code fails.
        if sys.platform == "darwin":
            def universal_cli(path):
                return package_release.UNIVERSAL if Path(path).name == "breccia" else ()
            with patched(package_release, mach_o_architectures=universal_cli, rosetta_problem=lambda: "no Rosetta here",
                         package_engine=lambda *a: sys.exit(9), package_sample=lambda *a: sys.exit(9)):
                code, out, err = call_main(package_release, ["--build", str(build), "--output", str(Path(temp, "a")), "--require-universal"])
                require(code == 1 and "the x86-64 half cannot be started on this Mac" in err and "no Rosetta here" in err, f"--require-universal without Rosetta: {code} {err}")
                code, out, err = call_main(package_release, ["--build", str(build), "--output", str(Path(temp, "b"))])
                require(code == 9 and "the x86-64 half of the tools and samples is not started: no Rosetta here" in out, f"a universal build without Rosetta: {code} {out}{err}")
            with patched(package_release, mach_o_architectures=lambda path: ()):
                code, out, err = call_main(package_release, ["--build", str(build), "--output", str(Path(temp, "c"))])
                require(code == 1 and "is not 64-bit Mach-O code" in err, f"a CLI that is no Mach-O code: {code} {err}")


def check_mach_o_parser(package_release):
    """mach_o_architectures on hand-made headers, on every platform."""
    with tempfile.TemporaryDirectory(prefix="BrecciaMachO-") as temp:
        def architectures(data):
            path = Path(temp, "file")
            path.write_bytes(data)
            return package_release.mach_o_architectures(path)
        thin_arm64 = b"\xcf\xfa\xed\xfe" + struct.pack("<i", 0x0100000C) + bytes(24)
        thin_x86_64 = b"\xcf\xfa\xed\xfe" + struct.pack("<i", 0x01000007)
        require(architectures(thin_arm64) == ("arm64",), "a thin arm64 header")
        require(architectures(thin_x86_64) == ("x86_64",), "a thin x86_64 header")
        fat = b"\xca\xfe\xba\xbe" + struct.pack(">I", 2) + struct.pack(">iiIII", 0x01000007, 3, 4096, 1, 12) + struct.pack(">iiIII", 0x0100000C, 0, 8192, 1, 14)
        require(architectures(fat) == ("arm64", "x86_64"), "a universal header")
        fat64 = b"\xca\xfe\xba\xbf" + struct.pack(">I", 2) + struct.pack(">iiQQII", 0x0100000C, 0, 4096, 1, 14, 0) + struct.pack(">iiQQII", 0x01000007, 3, 8192, 1, 12, 0)
        require(architectures(fat64) == ("arm64", "x86_64"), "a 64-bit universal header")
        require(architectures(b"\xca\xfe\xba\xbe" + struct.pack(">I", 1) + struct.pack(">iiIII", 18, 0, 4096, 1, 12)) == ("cpu-0x12",), "an unknown CPU type")
        # Sixteen slices is the most a universal file is taken to hold, so seventeen is refused (a Java class file's version,
        # read as the count, is 45 or more).
        entry = struct.pack(">iiIII", 0x0100000C, 0, 4096, 1, 14)
        require(architectures(b"\xca\xfe\xba\xbe" + struct.pack(">I", 16) + entry * 16) == ("arm64",), "sixteen slices")
        require(architectures(b"\xca\xfe\xba\xbe" + struct.pack(">I", 17) + entry * 17) == (), "seventeen slices")
        # A Java class file (minor and major version where the slice count would be), a truncated table, text, nothing, a
        # truncated header.
        for data in (b"\xca\xfe\xba\xbe\x00\x00\x00\x41", b"\xca\xfe\xba\xbe" + struct.pack(">I", 2) + bytes(20), b"stand-in", b"", fat[:7]):
            require(architectures(data) == (), f"not Mach-O: {data[:12]!r}")
    # The release name follows the CLI's architectures on macOS.
    if sys.platform == "darwin":
        with tempfile.TemporaryDirectory(prefix="BrecciaMachO-") as temp:
            Path(temp, "breccia").write_bytes(fat)
            require(package_release.platform_name(Path(temp, "breccia")) == "macos-universal", "a universal CLI")
            Path(temp, "breccia").write_bytes(thin_arm64)
            require(package_release.platform_name(Path(temp, "breccia")) == "macos-arm64", "an arm64 CLI")
            Path(temp, "breccia").write_bytes(thin_x86_64)
            require(package_release.platform_name(Path(temp, "breccia")) == "macos-x64", "an x86-64 CLI")


def main():
    cli = Path(sys.argv[1]).resolve()
    build = cli.parent.parent
    with_engine = "--with-engine" in sys.argv[2:]
    package_release = load("package_release", SCRIPT)
    version, platform = package_release.engine_version(), package_release.platform_name(cli)
    exe = package_release.EXE
    check_mach_o_parser(package_release)
    check_decisions(package_release, build)
    if sys.platform == "darwin":
        check_sign_macos_folder()

    # A tag must name the engine version.
    require(run(["--check-tag", f"v{version}"]).returncode == 0, "the matching tag was refused")
    mismatch = run(["--check-tag", "v999.0.0"])
    require(mismatch.returncode == 1 and "does not match the engine version" in mismatch.stderr, f"a mismatched tag: {mismatch.stderr}")
    require(run(["--build", str(build), "--output", "x", "--notarize", "profile"]).returncode == 2, "--notarize without --sign-identity")
    # Each platform's signing options are refused elsewhere.
    foreign = [["--windows-certificate", "0" * 40], ["--windows-dlib", "x.dll", "--windows-dlib-metadata", "x.json"]] if sys.platform != "win32" else []
    foreign += [["--sign-identity", "Developer ID Application: X"], ["--require-universal"]] if sys.platform != "darwin" else []
    for arguments in foreign:
        refused = run(["--build", str(build), "--output", "x"] + arguments)
        require(refused.returncode == 2 and ("is for" in refused.stderr or "are for" in refused.stderr), f"{arguments} on this platform: {refused.stderr}")
    for arguments, message in ((["--windows-certificate", "Z" * 40], "40 hexadecimal digits"), (["--windows-certificate", "0" * 41], "40 hexadecimal digits"),
                               (["--windows-timestamp-url", "http://t"], "goes with --windows-certificate"), (["--windows-certificate", "0" * 40, "--windows-dlib", "x"], "not allowed with"),
                               (["--windows-dlib", "x"], "go together")):
        refused = run(["--build", str(build), "--output", "x"] + arguments)
        require(refused.returncode == 2 and message in refused.stderr, f"{arguments}: {refused.stderr}")
    bad_level = run(["--build", str(build), "--output", "x", "--gzip-level", "0"])
    require(bad_level.returncode == 2 and "--gzip-level" in bad_level.stderr and "invalid choice" in bad_level.stderr, f"--gzip-level 0: {bad_level.stderr}")

    # archive()'s Linux branch, here on every host but Windows (no symbolic links without privileges): the gzip level
    # reaches the header (XFL, byte 8: 4 for level 1, 2 for level 9), and the tarball keeps the folder's name, the
    # executable bits and an AppDir's AppRun link.
    if sys.platform != "win32":
        with tempfile.TemporaryDirectory(prefix="BrecciaArchive-") as temp:
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

    with tempfile.TemporaryDirectory(prefix="Breccia Release ") as temp:
        env = dict(os.environ)
        signtool_log = Path(temp, "signtool.jsonl")
        windows_signing = []
        if sys.platform == "darwin":
            architectures = package_release.mach_o_architectures(cli)
            # Libraries of another architecture than the tools' fail the release, naming the file.
            # A MoltenVK of another architecture than the tools' (the loader is right) fails the release, naming the file:
            # in the tools archive, and in a sample's bundle.
            other = ("x86_64",) if architectures == ("arm64",) else ("arm64",)
            make_dylibs(Path(temp, "Other SDK", "lib"), architectures, other)
            for engine_too in ([True] if with_engine else []) + [False]:
                mismatch = run(["--build", str(build), "--output", str(Path(temp, f"mismatch {engine_too}")), "--samples", "Tetris"] + ([] if engine_too else ["--skip-engine"]),
                               dict(env, VULKAN_SDK=str(Path(temp, "Other SDK"))))
                expected = f"{'the tools' if engine_too else 'Tetris'}: libMoltenVK.dylib holds {', '.join(other)}, the tools {', '.join(architectures)}"
                require(mismatch.returncode == 1 and expected in mismatch.stderr, f"mismatched libraries: {mismatch.stdout[-2000:]}{mismatch.stderr}")
            if architectures != package_release.UNIVERSAL:
                refused = run(["--build", str(build), "--output", str(Path(temp, "refused")), "--require-universal"], env)
                require(refused.returncode == 1 and "the build is not universal" in refused.stderr, f"--require-universal: {refused.stderr}")
            # Export bundles the Vulkan loader and MoltenVK; headless smoke tests never load them, so empty ones do.
            make_dylibs(Path(temp, "Vulkan SDK", "lib"), architectures)
            env["VULKAN_SDK"] = str(Path(temp, "Vulkan SDK"))
        elif sys.platform == "win32":
            # A stand-in signtool records what scripts/sign-windows.py asks of it.
            windows_signing_test = load("WindowsSigning", Path(__file__).with_name("WindowsSigning.py"))
            env["SIGNTOOL"] = str(windows_signing_test.make_stand_in(Path(temp)))
            env["STANDIN_LOG"] = str(signtool_log)
            windows_signing = ["--windows-certificate", "ab" * 20]
        output = Path(temp, "dist")
        # The fastest gzip level: level 9 takes minutes over a Debug build's binaries. Only Linux writes .tar.gz archives
        # (other hosts zip and would warn that the option does nothing).
        linux = sys.platform.startswith("linux")
        arguments = ["--build", str(build), "--output", str(output), "--samples", "Tetris"] + (["--gzip-level", "1"] if linux else []) + ([] if with_engine else ["--skip-engine"]) + windows_signing
        packaged = run(arguments, env)
        require(packaged.returncode == 0, f"package_release.py failed: {packaged.stdout}{packaged.stderr}")
        # The packaged tools were started (under Rosetta too, on a universal build).
        require(("package_release: started the packaged tools\n" in packaged.stdout) == with_engine, f"the packaged tools were not started: {packaged.stdout[-2000:]}")
        universal_build = sys.platform == "darwin" and package_release.mach_o_architectures(cli) == package_release.UNIVERSAL
        require(("started the packaged tools under Rosetta" in packaged.stdout) == (with_engine and universal_build and not package_release.rosetta_problem()),
                f"the x86-64 tools were started or not when they should not have been: {packaged.stdout[-2000:]}")
        files = sorted(path.name for path in output.iterdir())
        checksums_name = f"SHA256SUMS-{platform}.txt"
        sample = next((name for name in files if name.startswith(f"Breccia-{version}-Tetris-{platform}")), None)
        engine = next((name for name in files if name.startswith(f"Breccia-{version}-{platform}.")), None)
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
            folder = f"Breccia-{version}-{platform}"
            contents = members(output / engine)
            wanted = {f"{folder}/bin/{tool}{exe}" for tool in package_release.TOOLS}
            wanted |= {f"{folder}/Samples/Tetris/Project.bproject", f"{folder}/Docs/Shipping.md", f"{folder}/LICENSE"}
            if sys.platform == "darwin":
                wanted |= {f"{folder}/bin/libvulkan.1.dylib", f"{folder}/bin/libMoltenVK.dylib", f"{folder}/bin/vulkan/icd.d/MoltenVK_icd.json"}
            require(wanted <= contents.keys(), f"tools archive lacks {sorted(wanted - contents.keys())}")
            if sys.platform == "darwin":
                # The manifest must reach the MoltenVK next to the tools.
                with zipfile.ZipFile(output / engine) as archive:
                    library = json.loads(archive.read(f"{folder}/bin/vulkan/icd.d/MoltenVK_icd.json"))["ICD"]["library_path"]
                require(posixpath.normpath(f"{folder}/bin/vulkan/icd.d/{library}") == f"{folder}/bin/libMoltenVK.dylib", f"the tools' manifest names {library}")

        if sys.platform == "darwin":
            # The tools and their libraries, and the sample's bundle, are signed (ad hoc here: flags 0x2, where the linker's
            # own signature would add linker-signed), and the signatures survive the archives.
            extracted = Path(temp, "extracted")
            code = [extracted / "Tetris.app"]
            subprocess.run(["ditto", "-x", "-k", str(output / sample), str(extracted)], check=True)
            if with_engine:
                subprocess.run(["ditto", "-x", "-k", str(output / engine), str(extracted)], check=True)
                code += [extracted / f"Breccia-{version}-{platform}" / "bin" / name for name in package_release.TOOLS + package_release.VULKAN_LIBRARIES]
            for path in code:
                verified = subprocess.run(["codesign", "--verify", "--strict", str(path)], capture_output=True, text=True)
                described = subprocess.run(["codesign", "-dv", str(path)], capture_output=True, text=True).stderr
                require(verified.returncode == 0 and "flags=0x2(adhoc)" in described, f"the packaged {path.name} is not signed ad hoc: {verified.stderr}{described}")
        if sys.platform == "win32":
            # The tools (with --with-engine) and the sample's executable, each signed and then verified.
            calls = [json.loads(line) for line in signtool_log.read_text().splitlines()]
            signed = sorted(Path(argument).name for call in calls if call[0] == "sign" for argument in call if argument.endswith(".exe"))
            expected = sorted(["Tetris.exe"] + (["breccia.exe", "BrecciaEditor.exe", "BrecciaRuntime.exe"] if with_engine else []))
            require(signed == expected and all("/sha1" in call and "ab" * 20 in call for call in calls if call[0] == "sign"), f"signtool calls: {calls}")
            require(["/d", "Tetris"] == next(call[call.index("/d"):call.index("/d") + 2] for call in calls if "/d" in call), f"the sample's description: {calls}")
            require(sum(call[0] == "verify" for call in calls) == 1 + with_engine, f"verifications: {calls}")

        # The output must be empty: archives are never mixed with an earlier run's.
        again = run(arguments, env)
        require(again.returncode == 1 and "is not empty" in again.stderr, f"a second run into the same output: {again.stderr}")
    print("ReleasePackaging: OK")


if __name__ == "__main__":
    main()
