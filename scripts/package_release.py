#!/usr/bin/env python3
"""Builds Basalt's release archives from a Dist build (the release workflow runs it on each platform).

  package_release.py --build <dir> --output <dir> [--samples Tetris,Breakout] [--skip-engine]
                     [--sign-identity <Developer ID> [--notarize <profile>]] [--require-universal]
                     [--windows-certificate <SHA-1 thumbprint>] [--require-appimage] [--gzip-level 1-9]
  package_release.py --check-tag <tag>

The first form writes, for the host platform:
  Basalt-<version>-<platform>.zip|.tar.gz   editor, runtime and CLI (bin/), Docs/, Samples/, README.md, LICENSE; on macOS
                                            also the Vulkan loader and MoltenVK, so the tools run without the Vulkan SDK
  Basalt-<version>-<Sample>-<platform>.*    each sample exported with project.export {"package": true, "archive": true}
                                            (its Assets in one Assets.bpak) and smoke-tested:
                                            a zipped .app, a zipped folder whose executable has icon and version
                                            resources, or an AppImage (an AppDir tarball when appimagetool is missing,
                                            unless --require-appimage)
  SHA256SUMS-<platform>.txt                 (the release workflow joins the platforms' lists into SHA256SUMS.txt)
<platform> is windows-x64, linux-x64, macos-arm64 or macos-universal (taken from the build's CLI on macOS).
Signing: on macOS the tools (with their Vulkan libraries) and each sample's .app are signed by scripts/sign-macos.sh,
with --sign-identity (and notarized with --notarize) or ad hoc. Every Mach-O file shipped must hold the architectures the
CLI holds, and --require-universal fails unless those are arm64 and x86_64; the x86-64 half is started through Rosetta
when the machine has it. On Windows --windows-certificate signs the tools and each sample's executable through
scripts/sign-windows.py. Signed tools and samples are started again afterwards.
--gzip-level sets the gzip level of the .tar.gz archives (Linux; default 9); the ReleasePackaging test passes 1 there,
since level 9 spends minutes on a Debug build's binaries. Other hosts write zip archives and ignore it, with a warning.
The second form exits 1 unless the tag is v<version> for the version in CMakeLists.txt.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent.parent
EXE = ".exe" if sys.platform == "win32" else ""
SMOKE_TEST_FRAMES = 60
# Sample files that are not part of a sample project (editor state, and replays the sample tests record).
IGNORED = shutil.ignore_patterns("Cache", "Saved", "imgui.ini", "*.log", ".DS_Store")


def fail(message):
    print(f"package_release: {message}", file=sys.stderr)
    sys.exit(1)


def engine_version():
    match = re.search(r"project\(Basalt\s+VERSION\s+([0-9.]+)", (ROOT / "CMakeLists.txt").read_text())
    if not match:
        fail("cannot find the project version in CMakeLists.txt")
    return match.group(1)


# Mach-O CPU types (mach/machine.h).
MACH_O_CPU_TYPES = {0x01000007: "x86_64", 0x0100000C: "arm64"}
UNIVERSAL = ["arm64", "x86_64"]


def mach_o_architectures(path):
    """The sorted architectures of a 64-bit Mach-O file, thin or universal; [] for any other file. What lipo -archs says,
    without needing the developer tools."""
    with open(path, "rb") as file:
        header = file.read(8)
        if len(header) < 8:
            return []
        if header[:4] == b"\xcf\xfa\xed\xfe":  # MH_MAGIC_64, little-endian on both architectures
            types = [struct.unpack("<i", header[4:8])[0]]
        elif header[:4] in (b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"):  # FAT_MAGIC, FAT_MAGIC_64: big-endian
            count = struct.unpack(">I", header[4:8])[0]
            size = 20 if header[3] == 0xBE else 32
            # Java class files share FAT_MAGIC; their next field is a version far above any real slice count.
            if count == 0 or count > 16:
                return []
            table = file.read(count * size)
            if len(table) < count * size:
                return []
            types = [struct.unpack(">i", table[i * size:i * size + 4])[0] for i in range(count)]
        else:
            return []
    return sorted(MACH_O_CPU_TYPES.get(cpu & 0xFFFFFFFF, f"cpu-{cpu & 0xFFFFFFFF:#x}") for cpu in types)


def platform_name(cli=None):
    """windows-x64, linux-x64 (or -arm64), macos-arm64 (or -x64) or macos-universal. On macOS the architectures come from
    the build's CLI when it is given, since a universal build runs on either."""
    system = {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux")
    if system == "macos" and cli is not None:
        architectures = mach_o_architectures(cli)
        if architectures == UNIVERSAL:
            return "macos-universal"
        machine = architectures[0] if len(architectures) == 1 else platform.machine()
    else:
        machine = platform.machine()
    machine = machine.lower()
    architecture = {"amd64": "x64", "x86_64": "x64", "arm64": "arm64", "aarch64": "arm64"}.get(machine, machine)
    return f"{system}-{architecture}"


def check_architectures(files, wanted, what):
    """Fails unless every file is Mach-O code for exactly the wanted architectures (those of the tools)."""
    for file in files:
        found = mach_o_architectures(file)
        if found != wanted:
            fail(f"{what}: {file.name} holds {', '.join(found) or 'no Mach-O code'}, the tools {', '.join(wanted)}: a Mac of "
                 "the missing architecture could not run it (build universal Vulkan libraries with scripts/ci-universal-vulkan.sh)")


def has_rosetta():
    """Whether this Mac can run x86-64 code (Rosetta 2 is installed, or the Mac is an Intel one)."""
    try:
        return subprocess.run(["arch", "-x86_64", "/usr/bin/true"], capture_output=True, timeout=60).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def warn(message):
    """A problem that does not stop the release, shown as an annotation on GitHub Actions."""
    print(f"{'::warning::' if os.environ.get('GITHUB_ACTIONS') else 'package_release: warning: '}{message}")


def run(arguments, what, **kwargs):
    """subprocess.run that fails with a message (and the output so far) when the program does not finish in time."""
    try:
        return subprocess.run(arguments, capture_output=True, text=True, **kwargs)
    except subprocess.TimeoutExpired as error:
        fail(f"{what} did not finish within {error.timeout} s: {error.stdout or ''}{error.stderr or ''}")


def run_cli(cli, project, command, params, env):
    """Runs one basalt command and returns its result; fails with the command's error otherwise. cli is the program, or
    the command line that starts it (arch -x86_64 basalt)."""
    program = [str(part) for part in cli] if isinstance(cli, list) else [str(cli)]
    completed = run(program + ["--project", str(project), command, json.dumps(params)], f"{project.name}: {command}", env=env, timeout=1800)
    # Log lines come first; the response is the last JSON object.
    start = completed.stdout.rfind("\n{")
    start = start + 1 if start >= 0 else completed.stdout.find("{")
    try:
        response = json.loads(completed.stdout[start:]) if start >= 0 else {}
    except json.JSONDecodeError:
        response = {}
    if completed.returncode != 0 or not response.get("ok") or "result" not in response:
        fail(f"{project.name}: {command} failed (exit code {completed.returncode}): {response.get('error') or completed.stdout + completed.stderr}")
    return response["result"]


def writes_zip(host=sys.platform):
    """Whether archive() zips on this host (Windows, macOS) rather than writing a .tar.gz."""
    return host in ("darwin", "win32")


def archive(source, destination, gzip_level, host=sys.platform):
    """Zips (Windows, macOS) or tars (Linux, keeping permissions and links; gzip_level 1-9) a file or directory, named
    after it."""
    if host == "darwin":
        # ditto keeps the bundle structure and its code signature (in the binaries and _CodeSignature/, not in extended
        # attributes); resource forks, extended attributes and ACLs are left out, so no Finder metadata ships.
        subprocess.run(["ditto", "-c", "-k", "--norsrc", "--noextattr", "--noacl", "--keepParent", str(source), f"{destination}.zip"], check=True)
        return Path(f"{destination}.zip")
    if host == "win32":
        return Path(shutil.make_archive(str(destination), "zip", root_dir=source.parent, base_dir=source.name))
    # What shutil.make_archive's gztar writes, at a chosen gzip level (it always uses 9).
    path = Path(f"{destination}.tar.gz")
    with tarfile.open(path, "w:gz", compresslevel=gzip_level) as file:
        file.add(source, arcname=source.name)
    return path


def find_vulkan_libraries():
    """libvulkan.1.dylib and libMoltenVK.dylib, searched where the exporter looks for them (FindVulkanLibraries in
    Basalt/Source/Basalt/Project/Exporter.cpp; it also looks next to the runtime, after $VULKAN_SDK): change both
    together."""
    directories = ([Path(os.environ["VULKAN_SDK"], "lib")] if os.environ.get("VULKAN_SDK") else []) + [Path("/opt/homebrew/lib"), Path("/usr/local/lib")]
    found = []
    for name in ("libvulkan.1.dylib", "libMoltenVK.dylib"):
        path = next((directory / name for directory in directories if (directory / name).is_file()), None)
        if not path:
            fail(f"cannot find {name} (install the Vulkan SDK or Homebrew vulkan-loader + molten-vk)")
        found.append(path.resolve())
    return found


TOOLS = ("BasaltEditor", "BasaltRuntime", "basalt")


def sign_macos(target, options):
    """scripts/sign-macos.sh on a bundle or a folder of loose code: with the Developer ID (and notarized) when given,
    ad hoc otherwise."""
    sign = ["bash", str(ROOT / "scripts" / "sign-macos.sh"), str(target)]
    if options.sign_identity:
        sign += ["--identity", options.sign_identity] + (["--notarize", options.notarize] if options.notarize else [])
    subprocess.run(sign, check=True)


def sign_windows(files, options, description=None):
    """scripts/sign-windows.py with the certificate whose thumbprint --windows-certificate gives."""
    sign = [sys.executable, str(ROOT / "scripts" / "sign-windows.py")] + [str(file) for file in files] + ["--thumbprint", options.windows_certificate]
    if description:
        sign += ["--description", description]
    subprocess.run(sign, check=True)


def start_tools(folder, sample, env, rosetta):
    """Starts the packaged CLI and runtime (signed now, so this checks that the signatures let them run), and on a
    universal build their x86-64 half through Rosetta. Any files they write stay in the staging folder, which is
    already archived."""
    project = folder / "Samples" / sample
    for prefix in [[]] + ([["arch", "-x86_64"]] if rosetta else []):
        what = f"the packaged tools{' under Rosetta' if prefix else ''}"
        run_cli(prefix + [str(folder / "bin" / f"basalt{EXE}")], project, "project.info", {}, env)
        smoke = run(prefix + [str(folder / "bin" / f"BasaltRuntime{EXE}"), "--project", str(project), "--headless", "--frames",
                              str(SMOKE_TEST_FRAMES), "--fail-on-script-errors"], f"{what}: BasaltRuntime", env=env, timeout=300)
        if smoke.returncode != 0:
            fail(f"{what}: BasaltRuntime exited with code {smoke.returncode} (7: script errors): {smoke.stdout}{smoke.stderr}")


def package_engine(build, stage, output, name, options, env):
    folder = stage / name
    bin_dir = folder / "bin"
    bin_dir.mkdir(parents=True)
    for tool in TOOLS:
        source = build / "bin" / f"{tool}{EXE}"
        if not source.is_file():
            fail(f"{source} is missing (build the Dist configuration first)")
        shutil.copy2(source, bin_dir)
    if sys.platform == "darwin":
        # The same layout a folder export uses: VulkanLoader finds the loader and the MoltenVK manifest next to the tools.
        loader, molten_vk = find_vulkan_libraries()
        shutil.copyfile(loader, bin_dir / "libvulkan.1.dylib")
        shutil.copyfile(molten_vk, bin_dir / "libMoltenVK.dylib")
        # Package-manager files are often read-only, and signing rewrites them.
        for library in ("libvulkan.1.dylib", "libMoltenVK.dylib"):
            (bin_dir / library).chmod(0o644)
        check_architectures([bin_dir / f"{tool}" for tool in TOOLS] + [bin_dir / "libvulkan.1.dylib", bin_dir / "libMoltenVK.dylib"],
                            options.architectures, "the tools")
        manifest = bin_dir / "vulkan" / "icd.d"
        manifest.mkdir(parents=True)
        (manifest / "MoltenVK_icd.json").write_text(json.dumps({"file_format_version": "1.0.0", "ICD": {"library_path": "../../libMoltenVK.dylib", "api_version": "1.3.0", "is_portability_driver": True}}))
        sign_macos(bin_dir, options)
    elif options.windows_certificate:
        sign_windows([bin_dir / f"{tool}{EXE}" for tool in TOOLS], options)
    shutil.copytree(ROOT / "Docs", folder / "Docs", ignore=IGNORED)
    shutil.copytree(ROOT / "Samples", folder / "Samples", ignore=IGNORED)
    for document in ("README.md", "LICENSE"):
        shutil.copy2(ROOT / document, folder)
    path = archive(folder, output / name, options.gzip_level)
    start_tools(folder, options.check_sample, env, sys.platform == "darwin" and options.architectures == UNIVERSAL and options.rosetta)
    return path


def package_sample(cli, sample, stage, output, prefix, options, env):
    export = stage / "exports" / sample.name
    result = run_cli(cli, sample, "project.export", {"output": str(export), "package": True, "archive": True, "smokeTestFrames": SMOKE_TEST_FRAMES}, env)
    package = Path(result["package"])
    for message in result.get("warnings", []):
        warn(f"{sample.name}: {message}")
    name = f"{prefix}-{sample.name}-{options.platform}"
    if sys.platform == "darwin":
        contents = package / "Contents"
        check_architectures([Path(result["executable"])] + sorted((contents / "Frameworks").glob("*.dylib")), options.architectures, sample.name)
        sign_macos(package, options)
        # What ships is the signed bundle (hardened runtime with a Developer ID): it must still start.
        run_cli(cli, sample, "project.smoke_test", {"executable": result["executable"], "frames": SMOKE_TEST_FRAMES, "assertPassed": True}, env)
        if options.architectures == UNIVERSAL and options.rosetta:
            smoke = run(["arch", "-x86_64", result["executable"], "--headless", "--frames", str(SMOKE_TEST_FRAMES), "--fail-on-script-errors"],
                        f"{sample.name} under Rosetta", env=env, timeout=300)
            if smoke.returncode != 0:
                fail(f"{sample.name}: the x86-64 half exited with code {smoke.returncode} under Rosetta (7: script errors): {smoke.stdout}{smoke.stderr}")
    elif package.suffix == ".AppImage":
        # The AppImage itself must start (extracted, so the check needs no FUSE).
        smoke = run([str(package), "--headless", "--frames", str(SMOKE_TEST_FRAMES), "--fail-on-script-errors"], f"{sample.name}: the AppImage",
                    env=dict(env, APPIMAGE_EXTRACT_AND_RUN="1"), timeout=300)
        if smoke.returncode != 0:
            fail(f"{sample.name}: the AppImage exited with code {smoke.returncode} (7: script errors): {smoke.stdout}{smoke.stderr}")
        target = output / f"{name}.AppImage"
        shutil.copy2(package, target)
        return target
    elif sys.platform.startswith("linux") and options.require_appimage:
        fail(f"{sample.name}: no AppImage was built: {'; '.join(result.get('warnings', []))}")
    elif options.windows_certificate:
        sign_windows([result["executable"]], options, description=sample.name)
        run_cli(cli, sample, "project.smoke_test", {"executable": result["executable"], "frames": SMOKE_TEST_FRAMES, "assertPassed": True}, env)
    return archive(package, output / name, options.gzip_level)


def write_checksums(files, destination):
    lines = []
    for file in sorted(files, key=lambda file: file.name):
        digest = hashlib.sha256()
        with open(file, "rb") as stream:
            for block in iter(lambda: stream.read(1 << 20), b""):
                digest.update(block)
        lines.append(f"{digest.hexdigest()}  {file.name}\n")
    destination.write_text("".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check-tag", metavar="TAG", help="exit 1 unless TAG is v<version in CMakeLists.txt>")
    parser.add_argument("--build", type=Path, help="the Dist build directory")
    parser.add_argument("--output", type=Path, help="where the archives go (created; must be empty)")
    parser.add_argument("--samples", help="comma-separated sample names (default: every project in Samples/)")
    parser.add_argument("--skip-engine", action="store_true", help="package only the samples")
    parser.add_argument("--sign-identity", help="macOS Developer ID for scripts/sign-macos.sh, for the tools and samples (default: ad hoc)")
    parser.add_argument("--notarize", metavar="PROFILE", help="notarize the macOS tools and samples with this notarytool keychain profile")
    parser.add_argument("--require-universal", action="store_true", help="fail on macOS unless the build is universal (arm64 and x86_64)")
    parser.add_argument("--windows-certificate", metavar="THUMBPRINT", help="sign the Windows tools and samples with this certificate (scripts/sign-windows.py)")
    parser.add_argument("--require-appimage", action="store_true", help="fail on Linux when appimagetool cannot build the AppImage")
    parser.add_argument("--gzip-level", type=int, choices=range(1, 10), metavar="1-9",
                        help="gzip level of the .tar.gz archives (Linux; default: 9)")
    options = parser.parse_args()

    version = engine_version()
    if options.check_tag is not None:
        if options.check_tag != f"v{version}":
            fail(f"the tag '{options.check_tag}' does not match the engine version {version} in CMakeLists.txt (tag v{version}, or change the version first)")
        print(f"package_release: the tag matches version {version}")
        return
    if not options.build or not options.output:
        parser.error("--build and --output are required (or use --check-tag)")
    if options.notarize and not options.sign_identity:
        parser.error("--notarize needs --sign-identity")
    if (options.sign_identity or options.require_universal) and sys.platform != "darwin":
        parser.error("--sign-identity and --require-universal are for macOS")
    if options.windows_certificate and sys.platform != "win32":
        parser.error("--windows-certificate is for Windows")
    if options.gzip_level is None:
        options.gzip_level = 9
    elif writes_zip():
        warn("--gzip-level has no effect here: this host writes zip archives")
    build = options.build.resolve()
    output = options.output.resolve()
    if output.exists() and any(output.iterdir()):
        fail(f"{output} is not empty")
    output.mkdir(parents=True, exist_ok=True)

    samples = sorted(path for path in (ROOT / "Samples").iterdir() if (path / "Project.bproject").is_file())
    if options.samples:
        wanted = options.samples.split(",")
        unknown = set(wanted) - {sample.name for sample in samples}
        if unknown:
            fail(f"unknown samples: {', '.join(sorted(unknown))}")
        samples = [sample for sample in samples if sample.name in wanted]

    cli = build / "bin" / f"basalt{EXE}"
    if not cli.is_file():
        fail(f"{cli} is missing (build the Dist configuration first)")
    options.platform = platform_name(cli)
    options.architectures = mach_o_architectures(cli) if sys.platform == "darwin" else []
    if options.require_universal and options.architectures != UNIVERSAL:
        fail(f"the build is not universal: {cli.name} holds {', '.join(options.architectures) or 'no Mach-O code'} "
             "(configure with -DCMAKE_OSX_ARCHITECTURES=\"arm64;x86_64\")")
    options.rosetta = options.architectures == UNIVERSAL and has_rosetta()
    if options.architectures == UNIVERSAL and not options.rosetta:
        warn("this Mac cannot run x86-64 code (no Rosetta 2): the x86-64 half of the tools and samples is not started")
    # The packaged tools start on the first sample packaged (the tools archive holds them all).
    options.check_sample = samples[0].name if samples else None

    prefix = f"Basalt-{version}"
    with tempfile.TemporaryDirectory(prefix="basalt-release-") as temp:
        stage = Path(temp)
        # Smoke tests save game data; keep it out of the user's (or the runner's) real data directory.
        env = dict(os.environ, BASALT_USER_DATA_DIR=str(stage / "UserData"))
        files = [] if options.skip_engine else [package_engine(build, stage, output, f"{prefix}-{options.platform}", options, env)]
        for sample in samples:
            files.append(package_sample(cli, sample, stage, output, prefix, options, env))
            print(f"package_release: {files[-1].name}")
    write_checksums(files, output / f"SHA256SUMS-{options.platform}.txt")
    print(f"package_release: {len(files)} archives in {output}")


if __name__ == "__main__":
    main()
