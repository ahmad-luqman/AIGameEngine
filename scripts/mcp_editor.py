#!/usr/bin/env python3
"""Bridge a standard MCP stdio client to BasaltEditor's loopback TCP transport.

Run the editor first, then configure the MCP client to launch this script. The bridge
forwards bytes without interpreting or executing commands; logs go only to stderr.
"""
import argparse
import socket
import sys
import threading


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=7420)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be in [1, 65535]")
    try:
        with socket.create_connection(("127.0.0.1", args.port), timeout=10) as connection:
            connection.settimeout(None)
            errors = []
            input_closed = threading.Event()

            def send():
                try:
                    while chunk := sys.stdin.buffer.read1(65536):
                        connection.sendall(chunk)
                    input_closed.set()
                    connection.shutdown(socket.SHUT_WR)
                except OSError as error:
                    errors.append(error)
                    try:
                        connection.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass  # Peer closure already wakes the receiver.

            # A peer disconnect must let the process exit even if the client still holds stdin open.
            sender = threading.Thread(target=send, daemon=True)
            sender.start()
            while chunk := connection.recv(65536):
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
            if not input_closed.is_set() and not errors:
                raise OSError("editor disconnected while the MCP client was still connected")
            if errors:
                raise errors[0]
    except OSError as error:
        print(f"basalt editor MCP: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
