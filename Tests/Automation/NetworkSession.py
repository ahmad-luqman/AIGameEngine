"""A network session over real UDP between BrecciaRuntime processes (ctest NetworkSession): a headless host on a port the
system picks, and a headless client that must connect, receive the networked entities and leave cleanly; a client of
another game, which the host refuses; one aimed at a port nobody answers on; one killed mid-session, which the host must
time out; and a host that stops on its own, which must tell its client. Clients that end unconnected exit with code 8.

Usage: NetworkSession.py <breccia CLI> <BrecciaRuntime>
"""
import json
import os
from pathlib import Path
import queue
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

TIMEOUT = 120
PORT_LINE = re.compile(r"Network: hosting on UDP port (\d+)")


def require(condition, message):
    if not condition:
        print(f"FAIL: {message}")
        sys.exit(1)


class Host:
    """A runtime hosting the project, its output read on a thread so its pipe never fills."""

    def __init__(self, runtime, project, env, frames=6000, role=("--host", "0")):
        # By default long enough for every client below; stopped once they are done.
        self.started = time.monotonic()
        self.process = subprocess.Popen([str(runtime), "--project", str(project), "--headless", "--frames", str(frames), *role],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
        self.lines = queue.Queue()
        self.output = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.process.stdout:
            self.output.append(line)
            self.lines.put(line)
        self.lines.put(None)

    def wait_for(self, pattern, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=max(deadline - time.monotonic(), 0.01))
            except queue.Empty:
                break
            if line is None:
                break
            match = re.search(pattern, line)
            if match:
                return match
        return None

    def text(self):
        return "".join(self.output)

    def stop(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=TIMEOUT)


BOB_SCRIPT = """local Bob = {}
function Bob:OnCreate() self.Time = 0 end
function Bob:OnUpdate(dt)
	self.Time = self.Time + dt
	self.Entity.Translation = Vec3(2, 1 + math.sin(self.Time * 3), 0)
end
return Bob
"""


def main():
    cli, runtime = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory() as temp:
        env = {**os.environ, "BRECCIA_USER_DATA_DIR": str(Path(temp) / "User")}

        def run(*arguments):
            # The log goes to stderr: one stream to search.
            return subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=TIMEOUT, env=env)

        def make_project(name):
            project = Path(temp) / name
            created = run(str(cli), "project.create", json.dumps({"path": str(project), "name": name}))
            require(created.returncode == 0, f"project.create: {created.stdout}")
            batch = Path(temp) / f"{name}.batch.json"
            batch.write_text(json.dumps([
                {"command": "scene.new", "params": {"name": "Main"}},
                {"command": "entity.create", "params": {"name": "Ground", "components": {
                    "Transform": {"Translation": [0, -0.5, 0]}, "RigidBody": {"Type": "Static"}, "BoxCollider": {"HalfExtents": [10, 0.5, 10]}}}},
                {"command": "entity.create", "params": {"name": "Crate", "components": {
                    "Transform": {"Translation": [0, 4, 0]}, "Mesh": {"Mesh": "builtin://Cube"}, "RigidBody": {"Type": "Dynamic"},
                    "BoxCollider": {}, "Networked": {}}}},
                # Keeps moving however late a client joins (the crate settles within a second).
                {"command": "asset.write", "params": {"path": "Assets/Scripts/Bob.lua", "content": BOB_SCRIPT}},
                {"command": "entity.create", "params": {"name": "Lamp", "components": {
                    "Transform": {"Translation": [2, 1, 0]}, "PointLight": {"Intensity": 5}, "Networked": {"Priority": 2},
                    "Script": {"Script": "Assets/Scripts/Bob.lua"}}}},
                {"command": "scene.save", "params": {"path": "Assets/Scenes/Main.bscene"}},
                {"command": "project.set", "params": {"startScene": "Assets/Scenes/Main.bscene", "network": {"TimeoutSeconds": 3}}},
            ]), encoding="utf-8")
            built = run(str(cli), "--project", str(project), "batch", str(batch))
            require(built.returncode == 0, f"building {name}: {built.stdout}")
            return project

        game = make_project("Arena")
        other = make_project("OtherGame")

        def client(project, address, frames=240):
            return run(str(runtime), "--project", str(project), "--headless", "--frames", str(frames), "--connect", address, "--fail-on-script-errors")

        host = Host(runtime, game, env)
        try:
            match = host.wait_for(PORT_LINE, 60)
            require(match is not None, f"the host never said which port it listens on:\n{host.text()}")
            port = int(match.group(1))
            require(port != 0, "the host reported port 0")

            joined = client(game, f"127.0.0.1:{port}")
            require(joined.returncode == 0, f"the client failed (exit {joined.returncode}):\n{joined.stdout}")
            require("connected to 127.0.0.1" in joined.stdout, f"the client never connected:\n{joined.stdout}")
            # The client's own copies of the networked entities make way for the host's.
            require("removed 2 networked entities" in joined.stdout, f"the client kept its own networked entities:\n{joined.stdout}")
            summary = re.search(r"Runtime: network client connections=1 remoteEntities=2 spawns=2 updates=(\d+) .* rejectedValues=0 rejectedRecords=0", joined.stdout)
            require(summary is not None, f"the client did not receive both networked entities cleanly:\n{joined.stdout}")
            # The lamp bobs on the host: its transform keeps arriving.
            require(int(summary.group(1)) > 5, f"the client received too few updates:\n{joined.stdout}")
            require(host.wait_for(r"Network: client 1 \(127\.0\.0\.1:\d+\) left: the client left", 30), f"the host did not hear the client leave:\n{host.text()}")

            refused = client(other, f"localhost:{port}")
            require(refused.returncode == 8, f"a client of another game should exit with 8, not {refused.returncode}:\n{refused.stdout}")
            require("different games" in refused.stdout, f"the client was not told why it was refused:\n{refused.stdout}")
            require(host.wait_for(r"disconnected a connecting client .*different games", 30), f"the host did not refuse the other game:\n{host.text()}")

            # A UDP port held open here, which never answers: the attempt fails after the project's 3-second timeout.
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as silent:
                silent.bind(("127.0.0.1", 0))
                unanswered = client(game, f"127.0.0.1:{silent.getsockname()[1]}", frames=360)
            require(unanswered.returncode == 8, f"a client that never connected should exit with 8, not {unanswered.returncode}:\n{unanswered.stdout}")
            require("not connected at the end of the run (the connection could not be made)" in unanswered.stdout, f"no reason given:\n{unanswered.stdout}")

            # A client that dies (no goodbye): the host times it out over real UDP.
            doomed = Host(runtime, game, env, frames=100000, role=("--connect", f"127.0.0.1:{port}"))
            try:
                joined_again = host.wait_for(r"Network: client (\d+) joined", 60)
                require(joined_again is not None, f"the second client never joined:\n{host.text()}\n{doomed.text()}")
            finally:
                doomed.stop()
            peer = joined_again.group(1)
            require(host.wait_for(rf"Network: client {peer} \(127\.0\.0\.1:\d+\) left: the connection timed out", 60), f"the host did not time out the killed client:\n{host.text()}")

            # --connect without a port uses the project's Network.Port.
            moved = run(str(cli), "--project", str(game), "project.set", json.dumps({"network": {"Port": port, "TimeoutSeconds": 3}}))
            require(moved.returncode == 0, f"project.set network: {moved.stdout}")
            defaulted = client(game, "127.0.0.1", frames=120)
            require(defaulted.returncode == 0, f"a client using the project's port failed (exit {defaulted.returncode}):\n{defaulted.stdout}")

            for option, value, message in [("--connect", "127.0.0.1:99999", "--connect must be"), ("--connect", ":7777", "--connect must be"),
                                           ("--connect", "127.0.0.1:", "--connect must be"), ("--connect", "127.0.0.1:0", "--connect must be"),
                                           ("--connect", "127.0.0.1:abc", "--connect must be"), ("--host", "-1", "--host must be"),
                                           ("--host", "65536", "--host must be"), ("--host", "abc", "--host must be")]:
                malformed = run(str(runtime), "--project", str(game), "--headless", "--frames", "1", option, value)
                require(malformed.returncode != 0 and message in malformed.stdout, f"{option} {value} was accepted:\n{malformed.stdout}")
            both = run(str(runtime), "--project", str(game), "--headless", "--frames", "1", "--host", "0", "--connect", "127.0.0.1")
            require(both.returncode != 0 and "cannot be combined" in both.stdout, f"--host and --connect together were accepted:\n{both.stdout}")
            taken = run(str(runtime), "--project", str(game), "--headless", "--frames", "1", "--host", str(port))
            require(taken.returncode == 1 and f"cannot listen on UDP port {port}" in taken.stdout, f"a second host on a port in use did not fail (exit {taken.returncode}):\n{taken.stdout}")
            require(host.process.poll() is None, f"the host stopped early:\n{host.text()}")
            # Nothing the clients sent was refused, nothing failed to spawn or send, and the socket never failed.
            for problem in ("sent a value", "cannot spawn", "could not send", "UDP socket failed", "invalid message", "not a networked entity"):
                require(problem not in host.text(), f"the host logged '{problem}':\n{host.text()}")
            for problem in ("not shown", "could not send", "UDP socket failed", "refuses"):
                require(problem not in joined.stdout, f"the client logged '{problem}':\n{joined.stdout}")
        finally:
            host.stop()

        # A host that stops on its own tells its client, which then ends unconnected. A headless host is paced: its 360
        # frames at 60 per second take about 6 seconds, not a fraction of one (timed alone, with the client running beside).
        brief = Host(runtime, game, env, frames=360)
        try:
            match = brief.wait_for(PORT_LINE, 60)
            require(match is not None, f"the brief host never said which port it listens on:\n{brief.text()}")
            ready = time.monotonic()
            left = Host(runtime, game, env, frames=900, role=("--connect", f"127.0.0.1:{match.group(1)}"))
            try:
                brief.process.wait(timeout=TIMEOUT)
                elapsed = time.monotonic() - ready
                left.process.wait(timeout=TIMEOUT)
            finally:
                left.stop()
            require(brief.process.returncode == 0, f"the brief host failed (exit {brief.process.returncode}):\n{brief.text()}")
            require(elapsed >= 4.0, f"the paced host ran its frames in {elapsed:.2f} s: the headless host is not paced")
            require(left.process.returncode == 8, f"a client whose host stopped should exit with 8, not {left.process.returncode}:\n{left.text()}")
            require("disconnected from the host: the host stopped" in left.text(), f"the client was not told the host stopped:\n{left.text()}")
        finally:
            brief.stop()
    print("OK")


if __name__ == "__main__":
    main()
