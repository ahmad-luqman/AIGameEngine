#!/usr/bin/env python3
"""Exercise the actual CLI framing, project session and stdout contract without a GPU."""
import json
import pathlib
import subprocess
import sys
import tempfile


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def request(identifier, method, params=None):
    value = {"jsonrpc": "2.0", "id": identifier, "method": method}
    if params is not None:
        value["params"] = params
    return value


def main():
    basalt = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="BasaltMcp-") as temp:
        messages = [
            request(1, "initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                     "clientInfo": {"name": "integration", "version": "1"}}),
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            request(2, "tools/list"),
            request(3, "tools/call", {"name": "project.create", "arguments": {"path": temp}}),
            request(4, "tools/call", {"name": "scene.new", "arguments": {"empty": True}}),
            request(5, "tools/call", {"name": "entity.create", "arguments": {"name": "MCP-created"}}),
            request(6, "tools/call", {"name": "asset.write", "arguments": {
                "path": "Assets/Scripts/Check.lua", "content": "print('check log'); return {}"}}),
            request(7, "tools/call", {"name": "script.check", "arguments": {"path": "Assets/Scripts/Check.lua"}}),
            request(8, "tools/call", {"name": "play.start"}),
            request(9, "tools/call", {"name": "lua.exec", "arguments": {"code": "print('runtime log')"}}),
            request(10, "tools/call", {"name": "play.stop"}),
            request(11, "tools/call", {"name": "entity.create", "arguments": {"name": False}}),
            request(12, "tools/call", {"name": "scene.save", "arguments": {"path": "Assets/Scenes/MCP.bscene"}}),
        ]
        payload = "\n".join(json.dumps(message) for message in messages) + "\n\n\r\n{bad}\n" + json.dumps(request(13, "ping")) + "\n"
        process = subprocess.run([basalt, "--verbose", "mcp"], input=payload, text=True, capture_output=True, timeout=60)
        require(process.returncode == 0, process.stderr)
        replies = [json.loads(line) for line in process.stdout.splitlines()]
        require(len(replies) == 16, f"Unexpected stdout messages: {process.stdout}")
        require(all(reply.get("jsonrpc") == "2.0" for reply in replies), "Non-MCP stdout")
        by_id = {reply["id"]: reply for reply in replies}
        require(sum(reply.get("error", {}).get("code") == -32700 for reply in replies) == 3, "Missing blank/malformed frame errors")
        require(by_id[13]["result"] == {}, "Did not recover after parse error")
        for identifier in range(3, 13):
            result = by_id[identifier]["result"]
            require(result["isError"] == (identifier == 11), f"Unexpected tool result {identifier}: {result}")
        require(by_id[5]["result"]["structuredContent"]["name"] == "MCP-created", "Missing structured result")
        require(pathlib.Path(temp, "Assets/Scenes/MCP.bscene").is_file(), "Scene was not persisted")
        # Clean EOF during Play must save earlier edit-mode work, and discard the play copy.
        edits = messages[:2] + [
            request(20, "tools/call", {"name": "entity.create", "arguments": {"name": "Saved-on-EOF"}}),
            request(21, "tools/call", {"name": "play.start"}),
            request(22, "tools/call", {"name": "entity.create", "arguments": {"name": "Play-only"}}),
        ]
        edit_payload = "\n".join(json.dumps(message) for message in edits) + "\n"
        command = [basalt, "--project", temp, "--scene", "Assets/Scenes/MCP.bscene", "mcp"]
        saved = subprocess.run(command, input=edit_payload, text=True, capture_output=True, timeout=60)
        require(saved.returncode == 0, saved.stderr)
        require(all(not reply.get("result", {}).get("isError", False)
                    for reply in map(json.loads, saved.stdout.splitlines())), saved.stdout)
        persisted = pathlib.Path(temp, "Assets/Scenes/MCP.bscene").read_text()
        require("Saved-on-EOF" in persisted and "Play-only" not in persisted, "EOF saved the wrong scene")
        partial = subprocess.run(command, input=edit_payload.replace("Saved-on-EOF", "Must-not-save") + "{",
                                 text=True, capture_output=True, timeout=60)
        require(partial.returncode != 0 and "incomplete final frame" in partial.stderr
                and "not auto-saved" in partial.stderr, "Missing transport/save diagnostics")
        require("Must-not-save" not in pathlib.Path(temp, "Assets/Scenes/MCP.bscene").read_text(),
                "Failed session unexpectedly auto-saved")
        help_result = subprocess.run([basalt, "help", "entity.create"], capture_output=True, text=True, check=True, timeout=30)
        schema = json.loads(help_result.stdout)["result"]["inputSchema"]
        tool = next(tool for tool in by_id[2]["result"]["tools"] if tool["name"] == "entity.create")
        require(tool["inputSchema"] == schema, "CLI and MCP schema drift")
        failure = subprocess.run([basalt, "--project", str(pathlib.Path(temp, "missing")), "mcp"], capture_output=True, text=True, timeout=30)
        require(failure.returncode != 0 and not failure.stdout, "Startup failure polluted MCP stdout")
        # Text that is not UTF-8 (here a Lua string) still prints as JSON, with replacement characters.
        invalid = [{"command": "scene.new", "params": {"empty": True}}, {"command": "play.start"},
                   {"command": "lua.exec", "params": {"code": "return 'bad \\255 byte'"}}]
        batch = subprocess.run([basalt, "--no-save", "batch", "-"], input=json.dumps(invalid).encode(), capture_output=True, timeout=60)
        require(batch.returncode == 0, batch.stderr.decode("utf-8", "replace"))
        replies = json.loads(batch.stdout.decode("utf-8"))
        require(replies[2]["result"]["result"] == "bad \ufffd byte", f"invalid UTF-8 output: {replies[2]}")
        single = subprocess.run([basalt, "lua.exec", json.dumps({"code": "return 1"})], capture_output=True, timeout=60)
        require(single.returncode != 0 and json.loads(single.stdout.decode("utf-8"))["ok"] is False, "one-shot output is not JSON")
    print("MCP stdio integration passed")


if __name__ == "__main__":
    main()
