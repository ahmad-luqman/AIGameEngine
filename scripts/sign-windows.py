#!/usr/bin/env python3
"""Signs Windows executables with signtool (Authenticode), so Windows names their publisher.

  sign-windows.py <file or folder>... (--thumbprint <SHA-1> | --dlib <dll> --dlib-metadata <json>)
                  [--timestamp-url <url> | --no-timestamp] [--description <text>] [--signtool <signtool.exe>]
                  [--allow-untrusted]

A folder stands for the .exe and .dll files directly in it: an exported game (sign it after every export, which never
keeps a signature) or the tools' bin/.

--thumbprint picks a code signing certificate from the current user's or the machine's certificate store. Certificates
on a hardware token or in a cloud HSM (where every certificate issued since June 2023 must keep its key) reach the store
through their vendor's key storage provider; a .pfx file goes there with
  Import-PfxCertificate -FilePath cert.pfx -CertStoreLocation Cert:\\CurrentUser\\My -Password (Read-Host -AsSecureString)
--dlib and --dlib-metadata hand signing to a signing service's library instead, such as Azure Trusted Signing's
(Azure.CodeSigning.Dlib.dll and its metadata.json; use --timestamp-url http://timestamp.acs.microsoft.com with it).

Signatures use SHA-256 and an RFC 3161 timestamp (default http://timestamp.digicert.com), which keeps them valid after
the certificate expires; a timestamp server that fails is tried again twice. --no-timestamp signs without one. Every file
is then checked with `signtool verify /pa`, which requires a certificate that chains to a trusted root; --allow-untrusted
skips that check, for test certificates only.

signtool comes from --signtool, the SIGNTOOL environment variable, PATH, or the newest Windows 10/11 SDK. It exits 1 when
signing or verifying fails, and 2 for bad arguments.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

DEFAULT_TIMESTAMP_URL = "http://timestamp.digicert.com"
# Seconds before each retry of a signing that failed for its timestamp (servers drop requests under load).
TIMESTAMP_RETRY_DELAYS = (2, 4)
SIGNTOOL_TIMEOUT = 600


def fail(message):
    print(f"sign-windows: {message}", file=sys.stderr)
    sys.exit(1)


def find_signtool(configured):
    """The signtool to run: --signtool, $SIGNTOOL, PATH, then the Windows SDK's newest for this machine."""
    for candidate in (configured, os.environ.get("SIGNTOOL")):
        if candidate:
            if not Path(candidate).is_file():
                fail(f"signtool '{candidate}' does not exist")
            return candidate
    found = shutil.which("signtool")
    if found:
        return found
    if sys.platform == "win32":
        # PROCESSOR_ARCHITECTURE rather than platform.machine(), which may start a process to find out.
        host = {"AMD64": "x64", "ARM64": "arm64", "x86": "x86"}.get(os.environ.get("PROCESSOR_ARCHITECTURE", ""), "x64")
        kits = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Windows Kits", "10", "bin")
        versions = sorted((path for path in kits.glob("10.*") if (path / host / "signtool.exe").is_file()),
                          key=lambda path: tuple(int(part) for part in path.name.split(".") if part.isdigit()))
        if versions:
            return str(versions[-1] / host / "signtool.exe")
    fail("cannot find signtool: install the Windows SDK, or pass --signtool or set SIGNTOOL")


def collect(paths):
    """Each file as given, and each folder's .exe and .dll files (not those of its subfolders), in order and once."""
    files = []
    for path in paths:
        if path.is_dir():
            found = sorted(entry for entry in path.iterdir() if entry.is_file() and entry.suffix.lower() in (".exe", ".dll"))
            if not found:
                fail(f"'{path}' holds no .exe or .dll file")
            files += found
        elif path.is_file():
            files.append(path)
        else:
            fail(f"'{path}' does not exist")
    seen = set()
    return [file for file in files if not (file.resolve() in seen or seen.add(file.resolve()))]


def run(arguments):
    try:
        return subprocess.run(arguments, capture_output=True, text=True, errors="replace", timeout=SIGNTOOL_TIMEOUT)
    except OSError as error:
        fail(f"cannot run '{arguments[0]}': {error}")
    except subprocess.TimeoutExpired:
        fail(f"signtool did not finish within {SIGNTOOL_TIMEOUT} s")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="+", type=Path, metavar="file or folder")
    parser.add_argument("--thumbprint", help="SHA-1 thumbprint of a code signing certificate in a certificate store")
    parser.add_argument("--dlib", help="a signing service's signtool library (e.g. Azure.CodeSigning.Dlib.dll)")
    parser.add_argument("--dlib-metadata", help="the JSON file that the --dlib library reads")
    parser.add_argument("--timestamp-url", default=DEFAULT_TIMESTAMP_URL, help=f"RFC 3161 timestamp server (default {DEFAULT_TIMESTAMP_URL})")
    parser.add_argument("--no-timestamp", action="store_true", help="sign without a timestamp (the signature ends with the certificate)")
    parser.add_argument("--description", help="the program name that Windows shows in its prompts (signtool /d)")
    parser.add_argument("--signtool", help="the signtool.exe to run")
    parser.add_argument("--allow-untrusted", action="store_true", help="skip `signtool verify /pa` (test certificates)")
    options = parser.parse_args()

    if bool(options.thumbprint) == bool(options.dlib):
        parser.error("pass either --thumbprint or --dlib")
    if bool(options.dlib) != bool(options.dlib_metadata):
        parser.error("--dlib and --dlib-metadata go together")
    if options.no_timestamp and options.timestamp_url != DEFAULT_TIMESTAMP_URL:
        parser.error("--no-timestamp and --timestamp-url contradict each other")
    if options.thumbprint and not (len(options.thumbprint) == 40 and all(c in "0123456789abcdefABCDEF" for c in options.thumbprint)):
        parser.error("--thumbprint must be 40 hexadecimal digits (a SHA-1 thumbprint, without spaces)")

    files = collect(options.paths)
    signtool = find_signtool(options.signtool)
    sign = [signtool, "sign", "/fd", "SHA256"]
    sign += ["/sha1", options.thumbprint] if options.thumbprint else ["/dlib", options.dlib, "/dmdf", options.dlib_metadata]
    if not options.no_timestamp:
        sign += ["/tr", options.timestamp_url, "/td", "SHA256"]
    if options.description:
        sign += ["/d", options.description]
    sign += [str(file) for file in files]

    for attempt in range(len(TIMESTAMP_RETRY_DELAYS) + 1):
        signed = run(sign)
        output = signed.stdout + signed.stderr
        if signed.returncode == 0:
            break
        # Only a timestamp server is worth asking again; any other failure (a wrong thumbprint, a locked file) stays.
        if "timestamp" not in output.lower() or attempt == len(TIMESTAMP_RETRY_DELAYS):
            fail(f"signtool sign exited with code {signed.returncode}: {output.strip()}")
        print(f"sign-windows: the timestamp server failed, trying again in {TIMESTAMP_RETRY_DELAYS[attempt]} s: {output.strip()}")
        time.sleep(TIMESTAMP_RETRY_DELAYS[attempt])

    if not options.allow_untrusted:
        verified = run([signtool, "verify", "/pa"] + [str(file) for file in files])
        if verified.returncode != 0:
            fail(f"signtool verify exited with code {verified.returncode} (is the certificate trusted on this machine?): {(verified.stdout + verified.stderr).strip()}")
    names = ", ".join(file.name for file in files)
    print(f"sign-windows: signed {names}{'' if options.no_timestamp else ' (timestamped)'}{'' if not options.allow_untrusted else ', not verified'}")


if __name__ == "__main__":
    main()
