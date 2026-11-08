#!/usr/bin/env python3
"""Linux regression: real EMFILE, repeated reconnects, and subsequent recovery.

Run after build.sh; only the child server gets the reduced descriptor limit.
All database changes and logs stay in a temporary directory.
"""
import argparse
import json
import os
from pathlib import Path
import resource
import socket
import subprocess
import tempfile
import time


def health(connection):
    connection.sendall(b"GET /health HTTP/1.1\r\nHost: test\r\n\r\n")
    data = bytearray()
    while b"\r\n\r\n" not in data:
        chunk = connection.recv(4096)
        if not chunk:
            raise AssertionError("connection closed before health response")
        data.extend(chunk)
    head, body = bytes(data).split(b"\r\n\r\n", 1)
    assert head.startswith(b"HTTP/1.1 200 "), head
    length = next(int(line.split(b":", 1)[1]) for line in head.split(b"\r\n")
                  if line.lower().startswith(b"content-length:"))
    while len(body) < length:
        chunk = connection.recv(4096)
        if not chunk:
            raise AssertionError("incomplete health response")
        body += chunk
    payload = json.loads(body)
    assert payload["status"] == "ok" and payload["db"] == "ok", payload


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, default=root / "bin/server")
    parser.add_argument("--limit", type=int, default=128)
    args = parser.parse_args()
    assert args.limit >= 64, "use at least 64 descriptors"
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    clients = []

    def limit_server():
        resource.setrlimit(resource.RLIMIT_NOFILE, (args.limit, args.limit))

    with tempfile.TemporaryDirectory(prefix="huykn-fd-") as directory:
        # Health uses SELECT 1; an empty database with the official schema is
        # sufficient and avoids a seed dependency for this transport test.
        import sqlite3
        path = Path(directory) / "feed.db"
        with sqlite3.connect(path) as db:
            db.executescript((root.parents[1] / "schema.sql").read_text())
        environment = dict(os.environ, HOST="127.0.0.1", PORT=str(port), SQLITE_PATH=str(path))
        with (Path(directory) / "server.log").open("w+") as log:
            process = subprocess.Popen([str(args.server.resolve())], env=environment,
                                       stdout=log, stderr=log, preexec_fn=limit_server)
            try:
                deadline = time.monotonic() + 10
                while True:
                    assert process.poll() is None, "server exited during startup"
                    try:
                        client = socket.create_connection(("127.0.0.1", port), timeout=1)
                        break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise
                        time.sleep(0.02)
                health(client)
                clients.append(client)
                peak = 0
                # Keep every client-side socket open. Only server-side pressure
                # relief can free enough descriptors to answer all these calls.
                for _ in range(args.limit * 3):
                    client = socket.create_connection(("127.0.0.1", port), timeout=3)
                    clients.append(client)
                    health(client)
                    peak = max(peak, len(os.listdir(f"/proc/{process.pid}/fd")))
                    assert process.poll() is None, "server exited on descriptor exhaustion"
                retired = sum(client.recv(1, socket.MSG_PEEK | socket.MSG_DONTWAIT) == b""
                              for client in clients[:-args.limit] if readable(client))
                assert peak >= args.limit - 4, f"descriptor pressure not reached: {peak}"
                assert retired > 0, "server did not retire any idle connections"
                assert process.poll() is None, "server exited after pressure"
                for client in clients:
                    client.close()
                clients.clear()
                with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
                    health(client)
                    health(client)  # Persistent connections still work after recovery.
                # Exhaust descriptors again with first requests still pending:
                # there are no completed keep-alives eligible for eviction.
                for _ in range(args.limit * 2):
                    clients.append(socket.create_connection(("127.0.0.1", port), timeout=3))
                time.sleep(0.05)
                assert process.poll() is None, "server exited with no idle eviction candidate"
                for client in clients[:4]:
                    health(client)  # Existing requests progress while accept backs off.
                for client in clients:
                    client.close()
                clients.clear()
                with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
                    health(client)
                print(f"PASS: limit={args.limit}, peak_fds={peak}, "
                      f"requests={args.limit * 3 + 8}, retired_idle={retired}, "
                      "no-idle recovery passed, server alive")
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read())
                raise
            finally:
                for client in clients:
                    client.close()
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def readable(connection):
    import select
    return bool(select.select([connection], [], [], 0)[0])


if __name__ == "__main__":
    main()
