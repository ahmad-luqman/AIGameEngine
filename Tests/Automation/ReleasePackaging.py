#!/usr/bin/env python3
"""Runs scripts/package_release.py the way the release workflow does, on one sample and without the tools archive."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "package_release.py"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(arguments, env=None):
    return subprocess.run([sys.executable, "-B", str(SCRIPT)] + arguments, capture_output=True, text=True, env=env, timeout=600)


def main():
    build = Path(sys.argv[1]).resolve().parent.parent
    version = re.search(r"project\(Basalt\s+VERSION\s+([0-9.]+)", (ROOT / "CMakeLists.txt").read_text()).group(1)

    # A tag must name the engine version.
    require(run(["--check-tag", f"v{version}"]).returncode == 0, "the matching tag was refused")
    mismatch = run(["--check-tag", "v999.0.0"])
    require(mismatch.returncode == 1 and "does not match the engine version" in mismatch.stderr, f"a mismatched tag: {mismatch.stderr}")
    require(run(["--build", str(build), "--output", "x", "--notarize", "profile"]).returncode == 2, "--notarize without --sign-identity")
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
        packaged = run(["--build", str(build), "--output", str(output), "--samples", "Tetris", "--skip-engine"], env)
        require(packaged.returncode == 0, f"package_release.py failed: {packaged.stdout}{packaged.stderr}")
        files = sorted(path.name for path in output.iterdir())
        platform = next(name for name in files if name.startswith("SHA256SUMS-"))[len("SHA256SUMS-"):-len(".txt")]
        archive = next((name for name in files if name.startswith(f"Basalt-{version}-Tetris-{platform}")), None)
        require(archive and len(files) == 2, f"unexpected release files {files}")
        checksums = Path(output, f"SHA256SUMS-{platform}.txt").read_text()
        require(re.fullmatch(rf"[0-9a-f]{{64}}  {re.escape(archive)}\n", checksums), f"checksums: {checksums!r}")
        # The output must be empty: archives are never mixed with an earlier run's.
        again = run(["--build", str(build), "--output", str(output), "--samples", "Tetris", "--skip-engine"], env)
        require(again.returncode == 1 and "is not empty" in again.stderr, f"a second run into the same output: {again.stderr}")
    print("ReleasePackaging: OK")


if __name__ == "__main__":
    main()
