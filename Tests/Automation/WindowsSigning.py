#!/usr/bin/env python3
"""scripts/sign-windows.py against a stand-in signtool on every platform; on Windows also with the real signtool and a
self-signed certificate, which checks that an export removes the runtime's signature and that the game can be signed.

Usage: WindowsSigning.py <basalt CLI>
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "sign-windows.py"
THUMBPRINT = "0123456789abcdef0123456789ABCDEF01234567"
# Records each call's arguments as one JSON line. STANDIN_FAIL makes calls fail: "timestamp:N" fails the first N sign
# calls as a timestamp server would, "network:N[:code]" likewise with a WinHTTP error that names no server, "sign" every sign
# call for another reason, "path" every sign call with an error that only mentions a path named timestamp, "verify" every
# verify call, "untrusted" every verify call with an untrusted root ("untrustedcode" by its error code alone, as Windows
# in another language says it), and "mixed" a verify call with an untrusted root for files whose name holds "Trusted"
# and another failure for the rest.
STAND_IN = r"""
import json, os, sys
log = os.environ["STANDIN_LOG"]
with open(log, "a") as file:
    file.write(json.dumps(sys.argv[1:]) + "\n")
mode, _, count = os.environ.get("STANDIN_FAIL", "").partition(":")
count, _, code = count.partition(":")
command = sys.argv[1]
if command == "sign" and mode in ("timestamp", "network"):
    calls = sum(1 for line in open(log) if json.loads(line)[0] == "sign")
    if calls <= int(count):
        if mode == "timestamp":
            print("SignTool Error: The specified timestamp server either could not be reached or returned an invalid response.")
        else:
            print(f"SignTool Error: An unexpected internal error has occurred. Error information: SignerSign() failed. ({code or '0x80072ee2'})")
        sys.exit(1)
untrusted_root = "SignTool Error: A certificate chain processed, but terminated in a root certificate which is not trusted by the trust provider."
if command == "verify" and (mode == "untrusted" or (mode == "mixed" and all("Trusted" in name for name in sys.argv[2:] if name.endswith(".exe")))):
    print(untrusted_root)
    sys.exit(1)
if command == "verify" and mode == "untrustedcode":
    print("SignTool Error: WinVerifyTrust returned error: 0x800B0109")
    sys.exit(1)
if command == "verify" and mode == "mixed":
    print("SignTool Error: WinVerifyTrust returned error: 0x80096010 The digital signature of the object did not verify.")
    sys.exit(1)
if command == "sign" and mode == "path":
    print("SignTool Error: The file C:\\timestamp\\Game.exe is being used by another process.")
    sys.exit(1)
if command == mode:
    print(f"SignTool Error: {command} failed for a reason of its own.")
    sys.exit(1)
print("Successfully done.")
"""


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def make_stand_in(directory):
    """A program that stands in for signtool: a Python script on its own (shebang) or behind a .cmd file on Windows."""
    script = directory / "signtool_standin.py"
    script.write_text(f"#!{sys.executable}\n{STAND_IN}")
    if sys.platform == "win32":
        wrapper = directory / "signtool.cmd"
        wrapper.write_text(f'@"{sys.executable}" "{script}" %*\r\n')
        return wrapper
    script.chmod(0o755)
    return script


def sign(arguments, env):
    return subprocess.run([sys.executable, "-B", str(SCRIPT)] + arguments, capture_output=True, text=True, env=env, timeout=120)


def take_calls(log):
    """The calls the stand-in recorded since the last take (the log is emptied)."""
    if not log.exists():
        return []
    entries = [json.loads(line) for line in log.read_text().splitlines()]
    log.unlink()
    return entries


def check_stand_in(temp):
    log = temp / "calls.jsonl"
    signtool = make_stand_in(temp)
    env = dict(os.environ, STANDIN_LOG=str(log), SIGNTOOL=str(signtool))
    env.pop("STANDIN_FAIL", None)
    folder = temp / "Game Folder"
    folder.mkdir()
    for name in ("Game.exe", "Helper.DLL", "readme.txt"):
        (folder / name).write_text("stand-in")
    (folder / "Nested").mkdir()
    (folder / "Nested" / "Inner.exe").write_text("stand-in")
    (temp / "Other").mkdir()
    (temp / "Other" / "Game.exe").write_text("stand-in")
    tool = temp / "Tool.exe"
    tool.write_text("stand-in")
    game, helper = str(folder / "Game.exe"), str(folder / "Helper.DLL")

    # A folder means its own .exe and .dll files; a file named twice is signed once. Verification checks the signer.
    signed = sign([str(folder), str(tool), str(folder / "Game.exe"), "--thumbprint", THUMBPRINT, "--description", "My Game"], env)
    require(signed.returncode == 0 and "signed Game.exe, Helper.DLL, Tool.exe (timestamped)" in signed.stdout, f"signing: {signed.stdout}{signed.stderr}")
    expected_sign = ["sign", "/fd", "SHA256", "/sha1", THUMBPRINT, "/tr", "http://timestamp.digicert.com", "/td", "SHA256", "/d", "My Game", game, helper, str(tool)]
    require(take_calls(log) == [expected_sign, ["verify", "/pa", "/sha1", THUMBPRINT, game, helper, str(tool)]], "signtool calls")
    # Files of the same name in two folders are two files.
    signed = sign([game, str(temp / "Other" / "Game.exe"), "--thumbprint", THUMBPRINT, "--no-timestamp"], env)
    require(signed.returncode == 0 and take_calls(log)[0][-2:] == [game, str(temp / "Other" / "Game.exe")], f"two files named Game.exe: {signed.stderr}")
    signed = sign([game, "--thumbprint", THUMBPRINT, "--machine-store", "--no-timestamp"], env)
    require(signed.returncode == 0 and take_calls(log)[0] == ["sign", "/fd", "SHA256", "/sm", "/sha1", THUMBPRINT, game], f"--machine-store: {signed.stderr}")

    # A signing service's library instead of a certificate, without a timestamp.
    signed = sign([game, "--dlib", "C:/Dlib.dll", "--dlib-metadata", "C:/metadata.json", "--no-timestamp"], env)
    require(signed.returncode == 0, f"--dlib: {signed.stdout}{signed.stderr}")
    require(take_calls(log) == [["sign", "/fd", "SHA256", "/dlib", "C:/Dlib.dll", "/dmdf", "C:/metadata.json", game], ["verify", "/pa", game]], "--dlib calls")
    signed = sign([game, "--thumbprint", THUMBPRINT, "--timestamp-url", "http://timestamp.acs.microsoft.com"], env)
    require(signed.returncode == 0 and take_calls(log)[0][5:9] == ["/tr", "http://timestamp.acs.microsoft.com", "/td", "SHA256"], f"--timestamp-url: {signed.stderr}")

    # --allow-untrusted accepts an untrusted root, and no other verification failure.
    signed = sign([game, "--thumbprint", THUMBPRINT, "--allow-untrusted"], dict(env, STANDIN_FAIL="untrusted"))
    require(signed.returncode == 0 and "does not trust" in signed.stdout, f"--allow-untrusted: {signed.stdout}{signed.stderr}")
    signed = sign([game, "--thumbprint", THUMBPRINT], dict(env, STANDIN_FAIL="untrusted"))
    require(signed.returncode == 1 and "is the certificate trusted" in signed.stderr, f"an untrusted root: {signed.stderr}")
    signed = sign([game, "--thumbprint", THUMBPRINT, "--allow-untrusted"], dict(env, STANDIN_FAIL="untrustedcode"))
    require(signed.returncode == 0 and "does not trust" in signed.stdout, f"an untrusted root named by its code: {signed.stdout}{signed.stderr}")
    signed = sign([game, "--thumbprint", THUMBPRINT, "--allow-untrusted"], dict(env, STANDIN_FAIL="verify"))
    require(signed.returncode == 1 and "verify failed for a reason of its own" in signed.stderr, f"--allow-untrusted with a broken signature: {signed.stderr}")
    # Each file is verified alone there, so one file's untrusted root does not cover another file's broken signature.
    trusted = temp / "Trusted.exe"
    trusted.write_text("stand-in")
    take_calls(log)
    signed = sign([str(trusted), game, "--thumbprint", THUMBPRINT, "--allow-untrusted"], dict(env, STANDIN_FAIL="mixed"))
    require(signed.returncode == 1 and "did not verify" in signed.stderr, f"--allow-untrusted with one broken signature among two: {signed.stdout}{signed.stderr}")
    require([call[:1] + call[4:] for call in take_calls(log)][1:] == [["verify", str(trusted)], ["verify", game]], "--allow-untrusted verifies file by file")
    signed = sign([str(trusted), "--thumbprint", THUMBPRINT, "--allow-untrusted"], dict(env, STANDIN_FAIL="mixed"))
    require(signed.returncode == 0 and "does not trust" in signed.stdout, f"--allow-untrusted on one untrusted file: {signed.stdout}{signed.stderr}")
    take_calls(log)

    # A timestamp server that fails, named or as a network error, is asked again; one that keeps failing fails the run.
    signed = sign([game, "--thumbprint", THUMBPRINT], dict(env, STANDIN_FAIL="network:1"))
    require(signed.returncode == 0 and signed.stdout.count("trying again in 5 s") == 1, f"a network error: {signed.stdout}{signed.stderr}")
    require([call[0] for call in take_calls(log)] == ["sign", "sign", "verify"], "calls with a network retry")
    signed = sign([game, "--thumbprint", THUMBPRINT], dict(env, STANDIN_FAIL="network:1:0x80072efd"))
    require(signed.returncode == 0 and len(take_calls(log)) == 3, f"a refused connection: {signed.stdout}{signed.stderr}")
    signed = sign([game, "--thumbprint", THUMBPRINT], dict(env, STANDIN_FAIL="timestamp:3"))
    require(signed.returncode == 1 and signed.stdout.count("trying again") == 2 and "timestamp server" in signed.stderr and len(take_calls(log)) == 3,
            f"a timestamp server that keeps failing: {signed.stdout}{signed.stderr}")
    # Without a timestamp there is no server to blame, and any other failure is not retried either.
    signed = sign([game, "--thumbprint", THUMBPRINT, "--no-timestamp"], dict(env, STANDIN_FAIL="network:1"))
    require(signed.returncode == 1 and len(take_calls(log)) == 1, f"--no-timestamp with a network error: {signed.stderr}")
    signed = sign([game, "--thumbprint", THUMBPRINT], dict(env, STANDIN_FAIL="sign"))
    require(signed.returncode == 1 and "sign failed for a reason of its own" in signed.stderr and len(take_calls(log)) == 1, f"a signing failure: {signed.stderr}")
    # The word in a path is no timestamp failure.
    signed = sign([game, "--thumbprint", THUMBPRINT], dict(env, STANDIN_FAIL="path"))
    require(signed.returncode == 1 and "being used by another process" in signed.stderr and len(take_calls(log)) == 1, f"a path naming timestamp: {signed.stderr}")
    signed = sign([game, "--thumbprint", THUMBPRINT], dict(env, STANDIN_FAIL="verify"))
    require(signed.returncode == 1 and "is the certificate trusted" in signed.stderr and "verify failed" in signed.stderr, f"a verification failure: {signed.stderr}")
    take_calls(log)

    # Invalid options exit 2 before anything runs; missing or empty inputs exit 1.
    for arguments, message in ((["--thumbprint", THUMBPRINT[:-1]], "40 hexadecimal digits"), (["--thumbprint", "Z" + THUMBPRINT[1:]], "40 hexadecimal digits"),
                               (["--thumbprint", THUMBPRINT + "0"], "40 hexadecimal digits"),
                               ([], "one of the arguments --thumbprint --dlib is required"),
                               (["--thumbprint", THUMBPRINT, "--dlib", "x", "--dlib-metadata", "y"], "not allowed with argument"),
                               (["--dlib", "x"], "go together"), (["--dlib", "x", "--dlib-metadata", "y", "--machine-store"], "goes with --thumbprint"),
                               (["--thumbprint", THUMBPRINT, "--no-timestamp", "--timestamp-url", "http://timestamp.digicert.com"], "not allowed with argument")):
        refused = sign([game] + arguments, env)
        require(refused.returncode == 2 and message in refused.stderr, f"{arguments}: {refused.stderr}")
    documents = temp / "Documents"
    documents.mkdir()
    (documents / "readme.txt").write_text("no code")
    for target, message in ((temp / "Missing.exe", "does not exist"), (documents, "holds no .exe or .dll")):
        refused = sign([str(target), "--thumbprint", THUMBPRINT], env)
        require(refused.returncode == 1 and message in refused.stderr, f"{target}: {refused.stderr}")
    refused = sign([game, "--thumbprint", THUMBPRINT, "--signtool", str(temp / "nowhere.exe")], env)
    require(refused.returncode == 1 and "does not exist" in refused.stderr, f"a missing signtool: {refused.stderr}")
    require(not log.exists(), "signtool ran for refused arguments")


def powershell(command):
    completed = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", command], capture_output=True, text=True, timeout=120)
    require(completed.returncode == 0, f"powershell {command}: {completed.stdout}{completed.stderr}")
    return completed.stdout.strip()


def signer(path):
    """The thumbprint of the certificate that signed a file, or 'NotSigned'."""
    return powershell(f"$s = Get-AuthenticodeSignature -LiteralPath '{path}'; if ($s.Status -eq 'NotSigned') {{ 'NotSigned' }} else {{ $s.SignerCertificate.Thumbprint }}")


def check_real_signtool(temp, cli):
    """The real signtool with a self-signed certificate: the runtime signed with it loses its signature in an export (folder
    and package), and the exported game signed with it still runs."""
    thumbprint = powershell("(New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Basalt Signing Test' -CertStoreLocation Cert:\\CurrentUser\\My -NotAfter (Get-Date).AddDays(1)).Thumbprint")
    try:
        tools = temp / "Tools"
        tools.mkdir()
        shutil.copy2(cli, tools / "basalt.exe")
        shutil.copy2(cli.parent / "BasaltRuntime.exe", tools / "BasaltRuntime.exe")
        runtime = tools / "BasaltRuntime.exe"
        signed = sign([str(runtime), "--thumbprint", thumbprint, "--no-timestamp", "--allow-untrusted"], dict(os.environ))
        if signed.returncode != 0 and "cannot find signtool" in signed.stderr and not os.environ.get("CI"):
            print("WindowsSigning: no signtool here (install the Windows SDK), so the real signing check is skipped")
            return
        require(signed.returncode == 0, f"signing the runtime: {signed.stdout}{signed.stderr}")
        require(signer(runtime) == thumbprint, "the runtime was not signed")

        env = dict(os.environ, BASALT_USER_DATA_DIR=str(temp / "User Data"))
        for package in (False, True):
            output = temp / f"Export {package}"
            exported = subprocess.run([str(tools / "basalt.exe"), "--project", str(ROOT / "Samples" / "Tetris"), "project.export",
                                       json.dumps({"output": str(output), "package": package, "smokeTestFrames": 10})], capture_output=True, text=True, env=env, timeout=600)
            # The response is the last JSON object, after any log lines.
            start = exported.stdout.rfind("\n{") + 1 if "\n{" in exported.stdout else exported.stdout.find("{")
            require(start >= 0, f"export (package {package}) printed no response: {exported.stdout[-2000:]}{exported.stderr}")
            response = json.loads(exported.stdout[start:])
            require(response.get("ok") and response["result"]["smokeTest"]["passed"], f"export (package {package}): {exported.stdout[-2000:]}{exported.stderr}")
            game = Path(response["result"]["executable"])
            require(signer(game) == "NotSigned", f"the export (package {package}) kept the runtime's signature")

        # What a developer does next: sign the game (the package, from the loop's last export), which still runs and keeps
        # its resources.
        signed = sign([str(game.parent), "--thumbprint", thumbprint, "--no-timestamp", "--allow-untrusted", "--description", "Tetris"], dict(os.environ))
        require(signed.returncode == 0 and signer(game) == thumbprint, f"signing the export: {signed.stdout}{signed.stderr}")
        smoke = subprocess.run([str(game), "--headless", "--frames", "10", "--fail-on-script-errors"], capture_output=True, text=True, env=env, timeout=300)
        require(smoke.returncode == 0, f"the signed game exited with {smoke.returncode}: {smoke.stdout[-2000:]}{smoke.stderr}")
        # The certificate chains to no trusted root, which the default verification refuses and --allow-untrusted (above)
        # accepts.
        untrusted = sign([str(game), "--thumbprint", thumbprint, "--no-timestamp"], dict(os.environ))
        require(untrusted.returncode == 1 and "is the certificate trusted" in untrusted.stderr, f"an untrusted certificate: {untrusted.stdout}{untrusted.stderr}")
    finally:
        powershell(f"Remove-Item -LiteralPath 'Cert:\\CurrentUser\\My\\{thumbprint}' -DeleteKey")


def main():
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="Basalt Signing ") as temp:
        check_stand_in(Path(temp))
        if sys.platform == "win32":
            check_real_signtool(Path(temp), cli)
    print("WindowsSigning: OK")


if __name__ == "__main__":
    main()
