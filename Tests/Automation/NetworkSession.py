"""A network session over real UDP between two BrecciaRuntime processes (ctest NetworkSession): a headless host on a port
the system picks, and a headless client that must connect, receive the networked entities and leave cleanly; then a
client of another game, which the host refuses, and one aimed at a port nobody answers on. Both end with exit code 8.

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

    def __init__(self, runtime, project, env):
        # Long enough for every client below; stopped once they are done.
        self.process = subprocess.Popen([str(runtime), "--project", str(project), "--headless", "--frames", "6000", "--host", "0"],
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
                    "Transform": {"Translation": [0, 4, 0]}, "RigidBody": {"Type": "Dynamic"}, "BoxCollider": {}, "Networked": {}}}},
                {"command": "entity.create", "params": {"name": "Lamp", "components": {
                    "Transform": {"Translation": [2, 1, 0]}, "PointLight": {"Intensity": 5}, "Networked": {"Priority": 2}}}},
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
            summary = re.search(r"Runtime: network client connections=1 remoteEntities=2 spawns=2 updates=(\d+)", joined.stdout)
            require(summary is not None, f"the client did not receive both networked entities:\n{joined.stdout}")
            # The crate falls on the host: its transform keeps arriving.
            require(int(summary.group(1)) > 5, f"the client received too few updates:\n{joined.stdout}")
            require(host.wait_for(r"Network: client 1 \(127\.0\.0\.1:\d+\) left: the client left", 30), f"the host did not hear the client leave:\n{host.text()}")

            refused = client(other, f"localhost:{port}")
            require(refused.returncode == 8, f"a client of another game should exit with 8, not {refused.returncode}:\n{refused.stdout}")
            require("different games" in refused.stdout, f"the client was not told why it was refused:\n{refused.stdout}")
            require(host.wait_for(r"disconnected a connecting client .*different games", 30), f"the host did not refuse the other game:\n{host.text()}")

            # A UDP port held open here, which never answers: the attempt never connects.
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as silent:
                silent.bind(("127.0.0.1", 0))
                unanswered = client(game, f"127.0.0.1:{silent.getsockname()[1]}", frames=60)
            require(unanswered.returncode == 8, f"a client that never connected should exit with 8, not {unanswered.returncode}:\n{unanswered.stdout}")
            require("not connected at the end of the run" in unanswered.stdout, f"no reason given:\n{unanswered.stdout}")

            malformed = run(str(runtime), "--project", str(game), "--headless", "--frames", "1", "--connect", "127.0.0.1:99999")
            require(malformed.returncode != 0 and "--connect must be" in malformed.stdout, f"a bad port was accepted:\n{malformed.stdout}")
            both = run(str(runtime), "--project", str(game), "--headless", "--frames", "1", "--host", "0", "--connect", "127.0.0.1")
            require(both.returncode != 0 and "cannot be combined" in both.stdout, f"--host and --connect together were accepted:\n{both.stdout}")
            require(host.process.poll() is None, f"the host stopped early:\n{host.text()}")
        finally:
            host.stop()
    print("OK")


if __name__ == "__main__":
    main()
