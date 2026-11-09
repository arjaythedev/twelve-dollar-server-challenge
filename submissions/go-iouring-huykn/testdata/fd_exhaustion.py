#!/usr/bin/env python3
"""Linux regression: held direct connections exceed the process descriptor limit.

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
    parser.add_argument("--connections", type=int, default=384)
    args = parser.parse_args()
    assert args.connections > args.limit, "connections must exceed the descriptor limit"
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
                # Spread client addresses across loopback IPs so the client's
                # ephemeral port range does not cap the held-connection test.
                for i in range(args.connections - 1):
                    source = (f"127.0.0.{2 + i // 20000}", 0)
                    client = socket.create_connection(("127.0.0.1", port), timeout=3,
                                                      source_address=source)
                    clients.append(client)
                    health(client)
                    peak = max(peak, len(os.listdir(f"/proc/{process.pid}/fd")))
                    assert process.poll() is None, "server exited on descriptor exhaustion"
                # Every socket must remain usable: idle eviction would fail here.
                for client in clients:
                    health(client)
                assert peak < args.limit, f"ordinary descriptor budget exhausted: {peak}"
                assert process.poll() is None, "server exited after held-connection check"
                print(f"PASS: limit={args.limit}, held_connections={len(clients)}, "
                      f"peak_process_fds={peak}, all keep-alives reusable")
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



if __name__ == "__main__":
    main()
