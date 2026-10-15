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
        help_result = subprocess.run([basalt, "help", "entity.create"], capture_output=True, text=True, check=True, timeout=30)
        schema = json.loads(help_result.stdout)["result"]["inputSchema"]
        tool = next(tool for tool in by_id[2]["result"]["tools"] if tool["name"] == "entity.create")
        require(tool["inputSchema"] == schema, "CLI and MCP schema drift")
        failure = subprocess.run([basalt, "--project", str(pathlib.Path(temp, "missing")), "mcp"], capture_output=True, text=True, timeout=30)
        require(failure.returncode != 0 and not failure.stdout, "Startup failure polluted MCP stdout")
    print("MCP stdio integration passed")


if __name__ == "__main__":
    main()
