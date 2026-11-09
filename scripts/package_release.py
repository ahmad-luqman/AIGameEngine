#!/usr/bin/env python3
"""Builds Basalt's release archives from a Dist build (the release workflow runs it on each platform).

  package_release.py --build <dir> --output <dir> [--samples Tetris,Breakout] [--skip-engine]
                     [--sign-identity <Developer ID> [--notarize <profile>]] [--require-universal]
                     [--windows-certificate <SHA-1 thumbprint> | --windows-dlib <dll> --windows-dlib-metadata <json>]
                     [--windows-timestamp-url <url>]
                     [--require-appimage] [--gzip-level 1-9]
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
<platform> is e.g. windows-x64, linux-x64, macos-arm64 or macos-universal (on macOS, from the build CLI's architectures).

Signing: on macOS the tools (with their Vulkan libraries) and each sample's .app are signed by scripts/sign-macos.sh,
with --sign-identity (and notarized with --notarize) or ad hoc. On Windows --windows-certificate (a certificate store
thumbprint) or --windows-dlib with --windows-dlib-metadata (a signing service) signs the tools' bin/ and each sample's
folder through scripts/sign-windows.py (timestamped by --windows-timestamp-url, or its default server). Each signed sample is smoke-tested again, and unless --skip-engine the packaged
CLI and runtime are started from the tools archive's folder once it is archived.
Architectures (macOS): every Mach-O file shipped must hold exactly the CLI's architectures. On a universal build the
x86-64 half of the packaged tools and of each sample is started through Rosetta 2; a Mac without it only warns, unless
--require-universal, which also fails unless the CLI holds both arm64 and x86_64.
--gzip-level sets the gzip level of the .tar.gz archives (Linux; default 9); the ReleasePackaging test passes 1 there,
since level 9 spends minutes on a Debug build's binaries. Other hosts write zip archives and ignore it, with a warning.
The second form exits 1 unless the tag is v<version> for the version in CMakeLists.txt.
"""
import argparse
from dataclasses import dataclass
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
TOOLS = ("BasaltEditor", "BasaltRuntime", "basalt")
VULKAN_LIBRARIES = ("libvulkan.1.dylib", "libMoltenVK.dylib")
# Mach-O CPU types (mach/machine.h).
MACH_O_CPU_TYPES = {0x01000007: "x86_64", 0x0100000C: "arm64"}
UNIVERSAL = ("arm64", "x86_64")
# Signing a bundle or folder may wait for Apple's notary service, which usually answers within minutes.
SIGN_TIMEOUT = 2 * 60 * 60


def fail(message):
    print(f"package_release: {message}", file=sys.stderr)
    sys.exit(1)


def engine_version():
    match = re.search(r"project\(Basalt\s+VERSION\s+([0-9.]+)", (ROOT / "CMakeLists.txt").read_text())
    if not match:
        fail("cannot find the project version in CMakeLists.txt")
    return match.group(1)


def mach_o_architectures(path):
    """The architectures of a 64-bit Mach-O file, thin or universal, sorted and unique; () for any other file. Slices
    are told apart by CPU type only (arm64e counts as arm64, other CPUs are cpu-0x...), as MachOArchitectures in
    Basalt/Source/Basalt/Project/Packaging.cpp reads them: enough to compare 64-bit Apple code without lipo."""
    with open(path, "rb") as file:
        header = file.read(8)
        if len(header) < 8:
            return ()
        if header[:4] == b"\xcf\xfa\xed\xfe":  # MH_MAGIC_64, little-endian on both architectures
            types = [struct.unpack("<I", header[4:8])[0]]
        elif header[:4] in (b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"):  # FAT_MAGIC, FAT_MAGIC_64: big-endian
            count = struct.unpack(">I", header[4:8])[0]
            size = 20 if header[3] == 0xBE else 32
            # Java class files share FAT_MAGIC; their next field is a version far above any real slice count.
            if count == 0 or count > 16:
                return ()
            table = file.read(count * size)
            if len(table) < count * size:
                return ()
            types = [struct.unpack(">I", table[i * size:i * size + 4])[0] for i in range(count)]
        else:
            return ()
    return tuple(sorted({MACH_O_CPU_TYPES.get(cpu, f"cpu-{cpu:#x}") for cpu in types}))


def platform_name(cli):
    """windows-x64, linux-x64 (or -arm64), macos-arm64 (or -x64) or macos-universal. On macOS the architectures come from
    the build's CLI, since a universal build runs on either: one architecture names the platform, arm64 + x86_64 is
    macos-universal, and any other set keeps the host's."""
    system = {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux")
    machine = platform.machine()
    if system == "macos":
        architectures = mach_o_architectures(cli)
        if architectures == UNIVERSAL:
            return "macos-universal"
        if len(architectures) == 1:
            machine = architectures[0]
    machine = machine.lower()
    architecture = {"amd64": "x64", "x86_64": "x64", "arm64": "arm64", "aarch64": "arm64"}.get(machine, machine)
    return f"{system}-{architecture}"


def check_architectures(files, wanted, what):
    """Fails unless every file is Mach-O code for exactly the wanted architectures (those of the tools)."""
    for file in files:
        if not file.is_file():
            fail(f"{what}: {file} is missing")
        found = mach_o_architectures(file)
        if found == wanted:
            continue
        missing = sorted(set(wanted) - set(found))
        if missing:
            detail = f"Macs of the missing kind ({', '.join(missing)}) could not run it (build universal Vulkan libraries with scripts/ci-universal-vulkan.sh)"
        else:
            detail = "it holds architectures the tools do not (use libraries built for the tools' architectures)"
        fail(f"{what}: {file.name} holds {', '.join(found) or 'no Mach-O code'}, the tools {', '.join(wanted)}: {detail}")


def rosetta_problem():
    """None when this Mac can run x86-64 code (Rosetta 2 is installed, or the Mac is an Intel one); otherwise why not."""
    try:
        probe = subprocess.run(["arch", "-x86_64", "/usr/bin/true"], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as error:
        return f"arch -x86_64 could not run: {error}"
    return None if probe.returncode == 0 else f"arch -x86_64 exited with code {probe.returncode} (is Rosetta 2 installed?): {probe.stderr.strip()}"


def warn(message):
    """A problem that does not stop the release, shown as an annotation on GitHub Actions."""
    print(f"{'::warning::' if os.environ.get('GITHUB_ACTIONS') else 'package_release: warning: '}{message}")


def run(arguments, what, **kwargs):
    """subprocess.run that fails with a message (and the output so far) when the program does not finish in time."""
    try:
        return subprocess.run(arguments, capture_output=True, text=True, **kwargs)
    except OSError as error:
        fail(f"{what} could not start: {error}")
    except subprocess.TimeoutExpired as error:
        # The partial output comes as bytes, whatever text=True asked for.
        partial = b"".join(part if isinstance(part, bytes) else part.encode() for part in (error.stdout or b"", error.stderr or b""))
        fail(f"{what} did not finish within {error.timeout} s: {partial.decode(errors='replace')}")


def run_smoke(command, what, env):
    """Runs a game, or the runtime on a project, headless for SMOKE_TEST_FRAMES frames; fails unless it exits 0."""
    smoke = run(command + ["--headless", "--frames", str(SMOKE_TEST_FRAMES), "--fail-on-script-errors"], what, env=env, timeout=300)
    if smoke.returncode != 0:
        fail(f"{what} exited with code {smoke.returncode} (7: script errors): {smoke.stdout}{smoke.stderr}")


def run_cli(cli, project, command, params, env, prefix=()):
    """Runs one basalt command and returns its result; fails with the command's error otherwise. prefix starts the CLI
    (arch -x86_64)."""
    completed = run([*prefix, str(cli), "--project", str(project), command, json.dumps(params)], f"{project.name}: {command}", env=env, timeout=1800)
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


def run_signing(command, what):
    """Runs a signing script, whose own output (on this script's stdout and stderr) explains a failure; fails with a
    message rather than a traceback. A timeout stops the script, not what it started (notarytool, signtool)."""
    # Our own lines first: the script writes to the same streams.
    sys.stdout.flush()
    try:
        signed = subprocess.run(command, timeout=SIGN_TIMEOUT)
    except OSError as error:
        fail(f"signing {what} could not start: {error}")
    except subprocess.TimeoutExpired:
        fail(f"signing {what} did not finish within {SIGN_TIMEOUT} s")
    if signed.returncode != 0:
        fail(f"signing {what} failed (exit code {signed.returncode}; the script's output above says why)")


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
    """libvulkan.1.dylib and libMoltenVK.dylib, searched where the exporter looks for them after the runtime's own
    directory (FindVulkanLibraries in Basalt/Source/Basalt/Project/Exporter.cpp): change both together."""
    directories = ([Path(os.environ["VULKAN_SDK"], "lib")] if os.environ.get("VULKAN_SDK") else []) + [Path("/opt/homebrew/lib"), Path("/usr/local/lib")]
    found = []
    for name in VULKAN_LIBRARIES:
        path = next((directory / name for directory in directories if (directory / name).is_file()), None)
        if not path:
            fail(f"cannot find {name} (install the Vulkan SDK or Homebrew vulkan-loader + molten-vk)")
        found.append(path.resolve())
    return found


@dataclass(frozen=True)
class Target:
    """What main() works out about the build and its signing before packaging it."""
    platform: str
    # The CLI's Mach-O architectures, which every shipped Mach-O file must hold exactly (macOS; () elsewhere).
    architectures: tuple[str, ...]
    # Whether the x86-64 half of a universal build is started too (through Rosetta 2).
    run_x86_64: bool
    # The sample whose project the packaged tools start on (the tools archive carries every sample in Samples/).
    check_sample: str
    # scripts/sign-windows.py's certificate arguments (--thumbprint, or --dlib and --dlib-metadata, then any
    # --timestamp-url); () signs nothing.
    windows_signing: tuple[str, ...] = ()

    def __post_init__(self):
        if self.run_x86_64 and self.architectures != UNIVERSAL:
            raise ValueError("only a universal build has an x86-64 half to start")


def sign_macos(path, options):
    """scripts/sign-macos.sh on a bundle or a folder of loose code: with the Developer ID (and notarized) when given,
    ad hoc otherwise."""
    sign = ["bash", str(ROOT / "scripts" / "sign-macos.sh"), str(path)]
    if options.sign_identity:
        sign += ["--identity", options.sign_identity] + (["--notarize", options.notarize] if options.notarize else [])
    run_signing(sign, f"'{path}'")


def sign_windows(paths, target, description=None):
    """scripts/sign-windows.py on files or folders (each folder's .exe and .dll files), with the target's certificate or
    signing service."""
    sign = [sys.executable, str(ROOT / "scripts" / "sign-windows.py")] + [str(path) for path in paths] + list(target.windows_signing)
    if description:
        sign += ["--description", description]
    run_signing(sign, ", ".join(Path(path).name for path in paths))


def start_tools(folder, target, env):
    """Starts the packaged CLI (project.info) and runtime (headless) on a sample, from the tools archive's folder and so
    after any signing (proving the signatures let them run), and on a universal build their x86-64 half through Rosetta.
    The editor needs a display and is not started. Runs once the folder is archived, so files they write do not ship."""
    project = folder / "Samples" / target.check_sample
    for prefix in [()] + ([("arch", "-x86_64")] if target.run_x86_64 else []):
        what = f"the packaged tools{' under Rosetta' if prefix else ''}"
        run_cli(folder / "bin" / f"basalt{EXE}", project, "project.info", {}, env, prefix)
        run_smoke([*prefix, str(folder / "bin" / f"BasaltRuntime{EXE}"), "--project", str(project)], f"{what}: BasaltRuntime", env)
        print(f"package_release: started {what}")


def package_engine(build, stage, output, name, options, target, env):
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
        # copyfile copies no mode, so the copies are writable for signing even when the originals are read-only.
        for source, library in zip(find_vulkan_libraries(), VULKAN_LIBRARIES):
            shutil.copyfile(source, bin_dir / library)
        check_architectures([bin_dir / file for file in TOOLS + VULKAN_LIBRARIES], target.architectures, "the tools")
        manifest = bin_dir / "vulkan" / "icd.d"
        manifest.mkdir(parents=True)
        (manifest / "MoltenVK_icd.json").write_text(json.dumps({"file_format_version": "1.0.0", "ICD": {"library_path": "../../libMoltenVK.dylib", "api_version": "1.3.0", "is_portability_driver": True}}))
        sign_macos(bin_dir, options)
    elif target.windows_signing:
        # The folder: every executable and library bin/ holds.
        sign_windows([bin_dir], target)
    shutil.copytree(ROOT / "Docs", folder / "Docs", ignore=IGNORED)
    shutil.copytree(ROOT / "Samples", folder / "Samples", ignore=IGNORED)
    for document in ("README.md", "LICENSE"):
        shutil.copy2(ROOT / document, folder)
    path = archive(folder, output / name, options.gzip_level)
    start_tools(folder, target, env)
    return path


def package_sample(cli, sample, stage, output, prefix, options, target, env):
    export = stage / "exports" / sample.name
    result = run_cli(cli, sample, "project.export", {"output": str(export), "package": True, "archive": True, "smokeTestFrames": SMOKE_TEST_FRAMES}, env)
    package = Path(result["package"])
    for message in result.get("warnings", []):
        warn(f"{sample.name}: {message}")
    name = f"{prefix}-{sample.name}-{target.platform}"
    smoke_test = {"executable": result["executable"], "frames": SMOKE_TEST_FRAMES, "assertPassed": True}
    if sys.platform == "darwin":
        frameworks = package / "Contents" / "Frameworks"
        check_architectures([Path(result["executable"])] + [frameworks / library for library in VULKAN_LIBRARIES], target.architectures, sample.name)
        sign_macos(package, options)
        # What ships is the signed bundle (ad hoc, or with a Developer ID and the hardened runtime): it must still start.
        run_cli(cli, sample, "project.smoke_test", smoke_test, env)
        if target.run_x86_64:
            run_smoke(["arch", "-x86_64", result["executable"]], f"{sample.name}: the x86-64 half under Rosetta", env)
    elif package.suffix == ".AppImage":
        # The AppImage itself must start (extracted, so the check needs no FUSE).
        run_smoke([str(package)], f"{sample.name}: the AppImage", dict(env, APPIMAGE_EXTRACT_AND_RUN="1"))
        destination = output / f"{name}.AppImage"
        shutil.copy2(package, destination)
        return destination
    elif sys.platform.startswith("linux") and options.require_appimage:
        fail(f"{sample.name}: no AppImage was built: {'; '.join(result.get('warnings', []))}")
    elif target.windows_signing:
        # The folder: every executable and library an export holds.
        sign_windows([package], target, description=sample.name)
        run_cli(cli, sample, "project.smoke_test", smoke_test, env)
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


def thumbprint(value):
    """argparse type: a SHA-1 thumbprint (scripts/sign-windows.py checks the same), refused before anything is packaged."""
    if not re.fullmatch(r"[0-9a-fA-F]{40}", value):
        raise argparse.ArgumentTypeError("a SHA-1 thumbprint is 40 hexadecimal digits, without spaces")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check-tag", metavar="TAG", help="exit 1 unless TAG is v<version in CMakeLists.txt>")
    parser.add_argument("--build", type=Path, help="the Dist build directory")
    parser.add_argument("--output", type=Path, help="where the archives go (created; must be empty)")
    parser.add_argument("--samples", help="comma-separated sample names (default: every project in Samples/)")
    parser.add_argument("--skip-engine", action="store_true", help="package only the samples")
    parser.add_argument("--sign-identity", help="macOS Developer ID for scripts/sign-macos.sh, for the tools and samples (default: ad hoc)")
    parser.add_argument("--notarize", metavar="PROFILE", help="notarize the macOS tools and samples with this notarytool keychain profile")
    parser.add_argument("--require-universal", action="store_true", help="fail on macOS unless the build is universal (arm64 and x86_64) and Rosetta 2 can start its x86-64 half")
    windows = parser.add_mutually_exclusive_group()
    windows.add_argument("--windows-certificate", metavar="THUMBPRINT", type=thumbprint, help="sign the Windows tools and samples with this certificate (scripts/sign-windows.py)")
    windows.add_argument("--windows-dlib", metavar="DLL", help="sign the Windows tools and samples through this signing service library (with --windows-dlib-metadata)")
    parser.add_argument("--windows-dlib-metadata", metavar="JSON", help="the metadata file that --windows-dlib reads")
    parser.add_argument("--windows-timestamp-url", metavar="URL", help="the RFC 3161 server that timestamps Windows signatures (default: scripts/sign-windows.py's)")
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
    if bool(options.windows_dlib) != bool(options.windows_dlib_metadata):
        parser.error("--windows-dlib and --windows-dlib-metadata go together")
    if options.windows_timestamp_url and not (options.windows_certificate or options.windows_dlib):
        parser.error("--windows-timestamp-url goes with --windows-certificate or --windows-dlib")
    if (options.windows_certificate or options.windows_dlib) and sys.platform != "win32":
        parser.error("--windows-certificate and --windows-dlib are for Windows")
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
    if not samples:
        fail("Samples/ holds no project to package, or to start the packaged tools on")

    cli = build / "bin" / f"basalt{EXE}"
    if not cli.is_file():
        fail(f"{cli} is missing (build the Dist configuration first)")
    architectures = mach_o_architectures(cli) if sys.platform == "darwin" else ()
    if sys.platform == "darwin" and not architectures:
        fail(f"{cli} is not 64-bit Mach-O code")
    if options.require_universal and architectures != UNIVERSAL:
        fail(f"the build is not universal: {cli.name} holds {', '.join(architectures)} (configure with -DCMAKE_OSX_ARCHITECTURES=\"arm64;x86_64\")")
    problem = rosetta_problem() if architectures == UNIVERSAL else None
    if problem and options.require_universal:
        fail(f"the x86-64 half cannot be started on this Mac, so it would ship untested: {problem}")
    if problem:
        warn(f"the x86-64 half of the tools and samples is not started: {problem}")
    if options.windows_certificate:
        windows_signing = ("--thumbprint", options.windows_certificate)
    elif options.windows_dlib:
        windows_signing = ("--dlib", options.windows_dlib, "--dlib-metadata", options.windows_dlib_metadata)
    else:
        windows_signing = ()
    if options.windows_timestamp_url:
        windows_signing += ("--timestamp-url", options.windows_timestamp_url)
    target = Target(platform=platform_name(cli), architectures=architectures, run_x86_64=architectures == UNIVERSAL and not problem,
                    check_sample=samples[0].name, windows_signing=windows_signing)

    prefix = f"Basalt-{version}"
    with tempfile.TemporaryDirectory(prefix="basalt-release-") as temp:
        stage = Path(temp)
        # Smoke tests save game data; keep it out of the user's (or the runner's) real data directory.
        env = dict(os.environ, BASALT_USER_DATA_DIR=str(stage / "UserData"))
        files = [] if options.skip_engine else [package_engine(build, stage, output, f"{prefix}-{target.platform}", options, target, env)]
        for sample in samples:
            files.append(package_sample(cli, sample, stage, output, prefix, options, target, env))
            print(f"package_release: {files[-1].name}")
    write_checksums(files, output / f"SHA256SUMS-{target.platform}.txt")
    print(f"package_release: {len(files)} archives in {output}")


if __name__ == "__main__":
    main()
