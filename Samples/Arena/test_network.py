"""The arena over real UDP (ctest SampleArenaNetwork): a headless BrecciaRuntime host (a dedicated server) and two
headless clients play one round with the computer playing (Autoplay.lua, added to a copy of the sample, with Match's
WinScore set). Player 1 tags player 2 until it wins; then every peer logs the scores and where it sees each character,
and quits. The scores must
match on every peer, and so must the positions (everyone is at rest by then).

Nothing here depends on how fast the machine is: the round starts whenever the clients join and ends at a score, the
host's wait for everyone to come to rest is counted in its fixed 1/60 s frames, and the host quits once the clients
have reported and left.

Usage: test_network.py <breccia CLI> <BrecciaRuntime>
"""
import json
import math
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
WIN_SCORE = 3
# One deadline for the whole session, inside ctest's TIMEOUT (300 s), so a hang is reported here with every peer's log.
SESSION_SECONDS = 240
# Frames each runtime may run at most, a bound rather than a timing: --frames fixes the timestep at 1/60 s, and a headless
# runtime in a session sleeps out each frame, so this is a minute of play (a round takes about 15 s), and somewhat more
# of wall time on a loaded machine, well inside the deadline. A peer that runs out of frames never logs "quitting".
MAX_FRAMES = 3600
NAMES = ("client 1", "client 2", "host")
PORT_LINE = re.compile(r"Network: hosting on UDP port (\d+)")
RESULT_LINE = re.compile(r"Arena: result peer=(\d+) scores=(\S*)((?: \S+=\S+)*)")
STATISTICS_LINE = re.compile(r"Runtime: network (?:host|client) .* stale=\d+ rejectedValues=(\d+) rejectedRecords=(\d+)")
# What no peer may log: refusals, failed sends, problems reported once (the engine's "(not logged again)"), and the
# sample's own warnings.
PROBLEMS = ("sent a value", "cannot spawn", "could not send", "could not ask", "could not show", "UDP socket failed",
            "invalid message", "refuses", "not shown", "not logged again", "dropped", "ignored", "cannot be", "faster than",
            "could not be put", "has not acknowledged", "may call it", "sent a tag", "the scoreboard received", "Script error",
            "is not replicated", "waits for its parent", "cannot go under", "is outside", "could not be sent", "second Welcome",
            "never sent", "cannot draw a tag", "nobody's player")


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
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.process.stdout:
            self.output.append(line)
            self.lines.put(line)
        self.lines.put(None)

    def wait_for(self, pattern, deadline):
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

    def finish(self, deadline):
        """The exit code, or None when the process had to be killed at the deadline. Its output is complete either way."""
        try:
            code = self.process.wait(timeout=max(deadline - time.monotonic(), 0.1))
        except subprocess.TimeoutExpired:
            code = None
        self.stop()
        return code

    def stop(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait()
        self.reader.join(timeout=30)
        require(not self.reader.is_alive(), "a runtime's output never ended")


def result(name, runtime):
    """The peer ID, scores and positions (name -> (x, y, z)) a runtime logged in its one result line."""
    matches = list(RESULT_LINE.finditer(runtime.text()))
    require(len(matches) == 1, f"the {name} logged {len(matches)} results, not one:\n{runtime.text()}")
    match = matches[0]
    entries = match.group(3).split()
    places = {}
    for entry in entries:
        entity, _, position = entry.partition("=")
        places[entity] = tuple(float(value) for value in position.split(","))
    require(len(places) == len(entries), f"the {name} reported two characters under one name: {entries}")
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
            {"command": "entity.create", "params": {"name": "Autoplay", "components": {"Script": {"Script": "Assets/Scripts/Autoplay.lua"}}}},
            # One round: its break lasts until everyone has reported.
            {"command": "component.set", "params": {"entity": "Match", "component": "Script", "data": {"Properties": {"WinScore": WIN_SCORE, "RoundBreak": 1000}}}},
            {"command": "scene.save"},
        ]), encoding="utf-8")
        # The CLI exits 1 when any request of the batch fails.
        built = subprocess.run([str(cli), "--project", str(project), "batch", str(batch)], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, timeout=60, env=env)
        require(built.returncode == 0, f"adding Autoplay: {built.stdout}")

        deadline = time.monotonic() + SESSION_SECONDS
        host = Runtime(runtime, project, env, "--host", "0")
        processes = {"host": host}
        try:
            # In the order the host logs them.
            match = host.wait_for(PORT_LINE, deadline)
            require(match is not None, f"the host never said which port it listens on:\n{host.text()}")
            require(host.wait_for(rf"Arena: autoplay to {WIN_SCORE}\b", deadline), f"the host does not play by itself to {WIN_SCORE}:\n{host.text()}")
            address = f"127.0.0.1:{match.group(1)}"
            # One at a time, so the first is peer 1 (the tagger) and the second peer 2.
            for peer in (1, 2):
                client = Runtime(runtime, project, env, "--connect", address)
                processes[f"client {peer}"] = client
                require(host.wait_for(rf"Arena: Player {peer} joined", deadline), f"client {peer} never joined:\n{host.text()}\n{client.text()}")
            codes = {name: processes[name].finish(deadline) for name in NAMES}
        finally:
            for process in processes.values():
                process.stop()

        for name, code in codes.items():
            require(code is not None, f"the {name} was still running after {SESSION_SECONDS} s:\n{processes[name].text()}")
            require(code == 0, f"the {name} failed (exit {code}):\n{processes[name].text()}")
            require("Arena: quitting" in processes[name].text(), f"the {name} ran out of frames instead of finishing:\n{processes[name].text()}")
        results = {name: result(name, processes[name]) for name in codes}
        peers = [results[name][0] for name in NAMES]
        require(peers == [1, 2, 0], f"peers reported as {peers}")
        expected_scores = f"1:{WIN_SCORE},2:0"
        for name, (_, scores, _) in results.items():
            require(scores == expected_scores, f"the {name} shows the scores {scores}, not {expected_scores}")
        host_places = results["host"][2]
        require(sorted(host_places) == ["BotA", "BotB", "Player1", "Player2"], f"the host reported {sorted(host_places)}")
        for name in NAMES[:2]:
            places = results[name][2]
            require(sorted(places) == sorted(host_places), f"the {name} sees {sorted(places)}, the host {sorted(host_places)}")
            for entity, position in host_places.items():
                require(math.dist(position, places[entity]) < 0.02, f"the {name} sees {entity} at {places[entity]}, the host at {position}")
        tagger = host_places["Player1"]
        require(abs(tagger[0] + 6) < 0.1 and abs(tagger[2] + 8) < 0.1, f"player 1 is not on its spot: {tagger}")
        require(host.text().count("Arena: Player 1 tagged Player2") == WIN_SCORE, f"the host did not count {WIN_SCORE} tags:\n{host.text()}")
        for name, process in processes.items():
            text = process.text()
            statistics = STATISTICS_LINE.search(text)
            require(statistics is not None, f"the {name} logged no network statistics:\n{text}")
            require(statistics.group(1) == "0" and statistics.group(2) == "0", f"the {name} rejected values or records: {statistics.group(0)}")
            for problem in PROBLEMS:
                require(problem not in text, f"the {name} logged '{problem}':\n{text}")
    print("OK")


if __name__ == "__main__":
    main()
