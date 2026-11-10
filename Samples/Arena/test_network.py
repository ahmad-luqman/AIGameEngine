"""The arena over real UDP (ctest SampleArenaNetwork): a headless BrecciaRuntime host (a dedicated server) and two
headless clients play one round with Match.Autoplay on, in a copy of the sample. Player 1 tags player 2 until it wins;
then every peer logs the scores and where it sees each character, and quits. The scores must match on every peer, and
so must the positions (everyone is at rest by then).

Nothing here depends on how fast the processes start or run: the round starts whenever the clients join, ends at a
score, and the host waits for the clients to report before it quits.

Usage: test_network.py <breccia CLI> <BrecciaRuntime>
"""
import json
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

SAMPLE = Path(__file__).resolve().parent
TIMEOUT = 180
# Frames each runtime may run at most (a headless runtime in a session is paced to 60 a second): a bound, not a timing.
MAX_FRAMES = 9000
PORT_LINE = re.compile(r"Network: hosting on UDP port (\d+)")
RESULT_LINE = re.compile(r"Arena: result peer=(\d+) scores=(\S*)((?: \S+=\S+)*)")


def require(condition, message):
    if not condition:
        print(f"FAIL: {message}")
        sys.exit(1)


class Runtime:
    """A headless runtime, its output read on a thread so its pipe never fills."""

    def __init__(self, runtime, project, env, *role):
        self.process = subprocess.Popen([str(runtime), "--project", str(project), "--headless", "--frames", str(MAX_FRAMES),
                                         "--fail-on-script-errors", *role],
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

    def finish(self):
        try:
            self.process.wait(timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=TIMEOUT)
        return self.process.returncode

    def stop(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=TIMEOUT)


def result(name, runtime):
    """The peer ID, scores and positions a runtime logged in its result line."""
    match = RESULT_LINE.search(runtime.text())
    require(match is not None, f"the {name} logged no result:\n{runtime.text()}")
    places = {}
    for entry in match.group(3).split():
        entity, _, position = entry.partition("=")
        places[entity] = tuple(float(value) for value in position.split(","))
    return int(match.group(1)), match.group(2), places


def main():
    cli, runtime = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory() as temp:
        env = {**os.environ, "BRECCIA_USER_DATA_DIR": str(Path(temp) / "User")}
        project = Path(temp) / "Arena"
        shutil.copytree(SAMPLE, project, ignore=shutil.ignore_patterns("Saved", "Cache", "__pycache__"))
        batch = Path(temp) / "autoplay.json"
        batch.write_text(json.dumps([
            {"command": "scene.open", "params": {"path": "Assets/Scenes/Arena.bscene"}},
            {"command": "component.set", "params": {"entity": "Match", "component": "Script", "data": {"Properties": {"Autoplay": True, "WinScore": 3}}}},
            {"command": "scene.save"},
        ]), encoding="utf-8")
        built = subprocess.run([str(cli), "--project", str(project), "batch", str(batch)], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, timeout=TIMEOUT, env=env)
        # A batch carries on past a failed request: every response must say ok.
        require(built.returncode == 0 and built.stdout.count('"ok": true') == 3, f"turning Autoplay on: {built.stdout}")

        host = Runtime(runtime, project, env, "--host", "0")
        clients = []
        try:
            match = host.wait_for(PORT_LINE, 60)
            require(match is not None, f"the host never said which port it listens on:\n{host.text()}")
            address = f"127.0.0.1:{match.group(1)}"
            # One at a time, so the first is peer 1 (the tagger) and the second peer 2.
            for peer in (1, 2):
                clients.append(Runtime(runtime, project, env, "--connect", address))
                require(host.wait_for(rf"Arena: Player {peer} joined", 60), f"client {peer} never joined:\n{host.text()}\n{clients[-1].text()}")
            codes = [client.finish() for client in clients] + [host.finish()]
        finally:
            for process in clients + [host]:
                process.stop()

        names = ["client 1", "client 2", "host"]
        for name, code, process in zip(names, codes, clients + [host]):
            require(code == 0, f"the {name} failed (exit {code}):\n{process.text()}")
        results = {name: result(name, process) for name, process in zip(names, clients + [host])}
        process_by_name = dict(zip(names, clients + [host]))
        require([results[name][0] for name in names] == [1, 2, 0], f"peers reported as {[results[name][0] for name in names]}")
        for name in names:
            require(results[name][1] == "1:3,2:0", f"the {name} shows the scores {results[name][1]}, not 1:3,2:0")
        host_places = results["host"][2]
        require(sorted(host_places) == ["BotA", "BotB", "Player1", "Player2"], f"the host reported {sorted(host_places)}")
        for name in names[:2]:
            places = results[name][2]
            require(sorted(places) == sorted(host_places), f"the {name} sees {sorted(places)}, the host {sorted(host_places)}")
            for entity, position in host_places.items():
                distance = sum((a - b) ** 2 for a, b in zip(position, places[entity])) ** 0.5
                require(distance < 0.02, f"the {name} sees {entity} at {places[entity]}, the host at {position}")
        tagger = host_places["Player1"]
        require(abs(tagger[0] + 6) < 0.1 and abs(tagger[2] + 8) < 0.1, f"player 1 is not on its spot: {tagger}")
        require(host.text().count("Arena: Player 1 tagged Player2") == 3, f"the host did not count three tags:\n{host.text()}")
        # Nothing was refused, failed to send or decode, and no tag was malformed.
        for problem in ("sent a value", "cannot spawn", "could not send", "UDP socket failed", "invalid message", "refuses", "not shown",
                        "is not a direction", "not a score table", "Script error"):
            for name, process in process_by_name.items():
                require(problem not in process.text(), f"the {name} logged '{problem}':\n{process.text()}")
    print("OK")


if __name__ == "__main__":
    main()
