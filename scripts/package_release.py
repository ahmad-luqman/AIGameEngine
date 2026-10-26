#!/usr/bin/env python3
"""Builds Basalt's release archives from a Dist build (the release workflow runs it on each platform).

  package_release.py --build <dir> --output <dir> [--samples Tetris,Breakout] [--skip-engine]
                     [--sign-identity <Developer ID> [--notarize <profile>]] [--require-appimage]
                     [--gzip-level 1-9]
  package_release.py --check-tag <tag>

The first form writes, for the host platform:
  Basalt-<version>-<platform>.zip|.tar.gz   editor, runtime and CLI (bin/), Docs/, Samples/, README.md, LICENSE; on macOS
                                            also the Vulkan loader and MoltenVK, so the tools run without the Vulkan SDK
  Basalt-<version>-<Sample>-<platform>.*    each sample exported with project.export {"package": true, "archive": true}
                                            (its Assets in one Assets.bpak) and smoke-tested:
                                            a zipped .app (signed, ad hoc without --sign-identity), a zipped folder whose
                                            executable has icon and version resources, or an AppImage (an AppDir tarball
                                            when appimagetool is missing, unless --require-appimage)
  SHA256SUMS-<platform>.txt                 (the release workflow joins the platforms' lists into SHA256SUMS.txt)
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


def platform_name():
    system = {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux")
    machine = platform.machine().lower()
    architecture = {"amd64": "x64", "x86_64": "x64", "arm64": "arm64", "aarch64": "arm64"}.get(machine, machine)
    return f"{system}-{architecture}"


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
    """Runs one basalt command and returns its result; fails with the command's error otherwise."""
    completed = run([str(cli), "--project", str(project), command, json.dumps(params)], f"{project.name}: {command}", env=env, timeout=1800)
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
    Basalt/Source/Basalt/Project/Exporter.cpp): change both together."""
    directories = ([Path(os.environ["VULKAN_SDK"], "lib")] if os.environ.get("VULKAN_SDK") else []) + [Path("/opt/homebrew/lib"), Path("/usr/local/lib")]
    found = []
    for name in ("libvulkan.1.dylib", "libMoltenVK.dylib"):
        path = next((directory / name for directory in directories if (directory / name).is_file()), None)
        if not path:
            fail(f"cannot find {name} (install the Vulkan SDK or Homebrew vulkan-loader + molten-vk)")
        found.append(path.resolve())
    return found


def package_engine(build, stage, output, name, options):
    folder = stage / name
    bin_dir = folder / "bin"
    bin_dir.mkdir(parents=True)
    for tool in ("BasaltEditor", "BasaltRuntime", "basalt"):
        source = build / "bin" / f"{tool}{EXE}"
        if not source.is_file():
            fail(f"{source} is missing (build the Dist configuration first)")
        shutil.copy2(source, bin_dir)
    if sys.platform == "darwin":
        # The same layout a folder export uses: VulkanLoader finds the loader and the MoltenVK manifest next to the tools.
        loader, molten_vk = find_vulkan_libraries()
        shutil.copyfile(loader, bin_dir / "libvulkan.1.dylib")
        shutil.copyfile(molten_vk, bin_dir / "libMoltenVK.dylib")
        manifest = bin_dir / "vulkan" / "icd.d"
        manifest.mkdir(parents=True)
        (manifest / "MoltenVK_icd.json").write_text(json.dumps({"file_format_version": "1.0.0", "ICD": {"library_path": "../../libMoltenVK.dylib", "api_version": "1.3.0", "is_portability_driver": True}}))
    shutil.copytree(ROOT / "Docs", folder / "Docs", ignore=IGNORED)
    shutil.copytree(ROOT / "Samples", folder / "Samples", ignore=IGNORED)
    for document in ("README.md", "LICENSE"):
        shutil.copy2(ROOT / document, folder)
    return archive(folder, output / name, options.gzip_level)


def package_sample(cli, sample, stage, output, prefix, options, env):
    export = stage / "exports" / sample.name
    result = run_cli(cli, sample, "project.export", {"output": str(export), "package": True, "archive": True, "smokeTestFrames": SMOKE_TEST_FRAMES}, env)
    package = Path(result["package"])
    for message in result.get("warnings", []):
        warn(f"{sample.name}: {message}")
    name = f"{prefix}-{sample.name}-{platform_name()}"
    if sys.platform == "darwin":
        sign = ["bash", str(ROOT / "scripts" / "sign-macos.sh"), str(package)]
        if options.sign_identity:
            sign += ["--identity", options.sign_identity] + (["--notarize", options.notarize] if options.notarize else [])
        subprocess.run(sign, check=True)
        # What ships is the signed bundle (hardened runtime with a Developer ID): it must still start.
        run_cli(cli, sample, "project.smoke_test", {"executable": result["executable"], "frames": SMOKE_TEST_FRAMES, "assertPassed": True}, env)
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
    parser.add_argument("--sign-identity", help="macOS Developer ID for scripts/sign-macos.sh (default: ad hoc)")
    parser.add_argument("--notarize", metavar="PROFILE", help="notarize the macOS samples with this notarytool keychain profile")
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

    prefix = f"Basalt-{version}"
    with tempfile.TemporaryDirectory(prefix="basalt-release-") as temp:
        stage = Path(temp)
        # Smoke tests save game data; keep it out of the user's (or the runner's) real data directory.
        env = dict(os.environ, BASALT_USER_DATA_DIR=str(stage / "UserData"))
        cli = build / "bin" / f"basalt{EXE}"
        files = [] if options.skip_engine else [package_engine(build, stage, output, f"{prefix}-{platform_name()}", options)]
        for sample in samples:
            files.append(package_sample(cli, sample, stage, output, prefix, options, env))
            print(f"package_release: {files[-1].name}")
    write_checksums(files, output / f"SHA256SUMS-{platform_name()}.txt")
    print(f"package_release: {len(files)} archives in {output}")


if __name__ == "__main__":
    main()
