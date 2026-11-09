#!/usr/bin/env python3
"""Signs Windows executables with signtool (Authenticode), so Windows names their publisher.

  sign-windows.py <file or folder>... (--thumbprint <SHA-1> [--machine-store] | --dlib <dll> --dlib-metadata <json>)
                  [--timestamp-url <url> | --no-timestamp] [--description <text>] [--signtool <signtool.exe>]
                  [--allow-untrusted]

A folder stands for the .exe and .dll files directly in it: an exported game (sign it after every export, which never
keeps a signature) or the tools' bin/.

--thumbprint picks a code signing certificate from the current user's certificate store (Cert:\\CurrentUser\\My), or
with --machine-store the machine's (Cert:\\LocalMachine\\My). Publicly trusted certificates issued since June 2023 keep
their key on a hardware token or in a cloud HSM, whose key storage provider puts them in a store; a .pfx file goes there
with
  Import-PfxCertificate -FilePath cert.pfx -CertStoreLocation Cert:\\CurrentUser\\My -Password (Read-Host -AsSecureString)
--dlib and --dlib-metadata hand signing to a signing service's library instead, such as Azure Trusted Signing's
(Azure.CodeSigning.Dlib.dll and its metadata.json; use --timestamp-url http://timestamp.acs.microsoft.com with it).

Signatures use SHA-256 and an RFC 3161 timestamp (default http://timestamp.digicert.com), which keeps them valid after
the certificate expires. A signing that fails because of the timestamp server (signtool names it, or reports a network
error) is tried again twice, after 5 and 15 seconds; --no-timestamp signs without one. Every file is then checked with
`signtool verify /pa` (and, with --thumbprint, that this certificate signed it), which requires a certificate that
chains to a root this machine trusts; --allow-untrusted accepts an untrusted root and nothing else, for test certificates.

signtool comes from --signtool, the SIGNTOOL environment variable, PATH, or the newest Windows 10/11 SDK. It exits 2 for
invalid options, and 1 when a file, a folder's code or signtool is missing, or signing or verifying fails.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

DEFAULT_TIMESTAMP_URL = "http://timestamp.digicert.com"
# Seconds before each retry of a signing that failed for its timestamp (servers drop requests under load).
TIMESTAMP_RETRY_DELAYS = (5, 15)
# WinHTTP errors that signtool reports for a timestamp server it cannot reach, without naming the server: timeout, name
# not resolved, connection refused or reset, and a TLS failure.
TIMESTAMP_NETWORK_ERRORS = ("0x80072ee2", "0x80072ee7", "0x80072efd", "0x80072eff", "0x80072f8f")
# What `signtool verify /pa` says about a certificate that chains to an untrusted root (CERT_E_UNTRUSTEDROOT).
UNTRUSTED_ROOT = ("0x800b0109", "terminated in a root certificate which is not trusted")
SIGNTOOL_TIMEOUT = 600


def fail(message):
    print(f"sign-windows: {message}", file=sys.stderr)
    sys.exit(1)


def thumbprint(value):
    """argparse type: a SHA-1 thumbprint, 40 hexadecimal digits."""
    if not re.fullmatch(r"[0-9a-fA-F]{40}", value):
        raise argparse.ArgumentTypeError("a SHA-1 thumbprint is 40 hexadecimal digits, without spaces")
    return value


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
    unique, seen = [], set()
    for file in files:
        key = file.resolve()
        if key not in seen:
            seen.add(key)
            unique.append(file)
    return unique


def run(arguments):
    try:
        completed = subprocess.run(arguments, capture_output=True, text=True, errors="replace", timeout=SIGNTOOL_TIMEOUT)
    except OSError as error:
        fail(f"cannot run '{arguments[0]}': {error}")
    except subprocess.TimeoutExpired as error:
        fail(f"signtool did not finish within {SIGNTOOL_TIMEOUT} s: {error.stdout or ''}{error.stderr or ''}")
    return completed, (completed.stdout + completed.stderr).strip()


def is_timestamp_failure(output):
    lowered = output.lower()
    return "timestamp" in lowered or any(code in lowered for code in TIMESTAMP_NETWORK_ERRORS)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="+", type=Path, metavar="file or folder")
    certificate = parser.add_mutually_exclusive_group(required=True)
    certificate.add_argument("--thumbprint", type=thumbprint, help="SHA-1 thumbprint of a code signing certificate in a certificate store")
    certificate.add_argument("--dlib", help="a signing service's signtool library (e.g. Azure.CodeSigning.Dlib.dll)")
    parser.add_argument("--dlib-metadata", help="the JSON file that the --dlib library reads")
    parser.add_argument("--machine-store", action="store_true", help="look for --thumbprint in the machine's store, not the current user's")
    timestamp = parser.add_mutually_exclusive_group()
    timestamp.add_argument("--timestamp-url", help=f"RFC 3161 timestamp server (default {DEFAULT_TIMESTAMP_URL})")
    timestamp.add_argument("--no-timestamp", action="store_true", help="sign without a timestamp (the signature ends with the certificate)")
    parser.add_argument("--description", help="the program name that Windows shows in its prompts (signtool /d)")
    parser.add_argument("--signtool", help="the signtool.exe to run")
    parser.add_argument("--allow-untrusted", action="store_true", help="accept a certificate whose root this machine does not trust (test certificates)")
    options = parser.parse_args()
    if bool(options.dlib) != bool(options.dlib_metadata):
        parser.error("--dlib and --dlib-metadata go together")
    if options.machine_store and not options.thumbprint:
        parser.error("--machine-store goes with --thumbprint")

    files = collect(options.paths)
    names = [str(file) for file in files]
    signtool = find_signtool(options.signtool)
    store = ["/sm"] if options.machine_store else []
    sign = [signtool, "sign", "/fd", "SHA256"]
    sign += store + ["/sha1", options.thumbprint] if options.thumbprint else ["/dlib", options.dlib, "/dmdf", options.dlib_metadata]
    if not options.no_timestamp:
        sign += ["/tr", options.timestamp_url or DEFAULT_TIMESTAMP_URL, "/td", "SHA256"]
    if options.description:
        sign += ["/d", options.description]

    for delay in TIMESTAMP_RETRY_DELAYS + (None,):
        signed, output = run(sign + names)
        if signed.returncode == 0:
            break
        # Only a failure of the timestamp server is worth asking again; any other (a wrong thumbprint, a locked file) stays.
        if delay is None or options.no_timestamp or not is_timestamp_failure(output):
            fail(f"signtool sign exited with code {signed.returncode}: {output}")
        print(f"sign-windows: the timestamp server failed, trying again in {delay} s: {output}")
        time.sleep(delay)

    # /sha1 here checks that the certificate asked for signed each file, not merely some trusted one.
    verified, output = run([signtool, "verify", "/pa"] + (["/sha1", options.thumbprint] if options.thumbprint else []) + names)
    untrusted = verified.returncode != 0 and options.allow_untrusted and any(text in output.lower() for text in UNTRUSTED_ROOT)
    if verified.returncode != 0 and not untrusted:
        fail(f"signtool verify exited with code {verified.returncode} (is the certificate trusted on this machine?): {output}")
    print(f"sign-windows: signed {', '.join(file.name for file in files)}{'' if options.no_timestamp else ' (timestamped)'}"
          f"{', by a certificate this machine does not trust' if untrusted else ''}")


if __name__ == "__main__":
    main()
