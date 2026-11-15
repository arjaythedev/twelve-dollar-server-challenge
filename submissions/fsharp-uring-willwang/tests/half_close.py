"""Run with Python 3 on Linux: half_close.py SERVER SEED_DB TOKENS_JSON."""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import sqlite3
import subprocess
import tempfile
import time


def request(path, *, token=None, body=b""):
    method = "POST" if token is not None else "GET"
    auth = f"Authorization: Bearer {token}\r\n" if token is not None else ""
    return (f"{method} {path} HTTP/1.1\r\nHost: localhost\r\n{auth}"
            f"Content-Type: application/json\r\nContent-Length: {len(body)}\r\n\r\n").encode() + body


def response(reader, status):
    line = reader.readline()
    if not line or int(line.split()[1]) != status:
        raise AssertionError(f"expected HTTP {status}, received {line!r}")
    headers = {}
    for line in iter(reader.readline, b"\r\n"):
        if not line:
            raise EOFError("truncated HTTP headers")
        key, value = line.split(b":", 1)
        headers[key.lower()] = value.strip()
    return json.loads(reader.read(int(headers[b"content-length"])))


def check(server, seed, tokens):
    token = json.loads(tokens.read_bytes())[0]["token"]
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        addr = probe.getsockname()
    with tempfile.TemporaryDirectory() as tmp:
        db = Path(tmp) / "feed.db"
        shutil.copyfile(seed, db)
        env = dict(os.environ, HOST=addr[0], PORT=str(addr[1]), SQLITE_PATH=str(db),
                   JWT_SECRET="twelve-dollar-challenge")
        app = subprocess.Popen([server], cwd=tmp, env=env, start_new_session=True)
        try:
            until = time.monotonic() + 15
            while True:
                try:
                    older = socket.create_connection(addr, timeout=3)
                    break
                except ConnectionRefusedError:
                    if app.poll() is not None or time.monotonic() >= until:
                        raise
                    time.sleep(0.05)
            with older, older.makefile("rb") as old_reader:
                older.sendall(request("/health"))
                assert response(old_reader, 200)["db"] == "ok"
                with socket.create_connection(addr, timeout=3) as peer, peer.makefile("rb") as reader:
                    peer.sendall(request("/health"))
                    assert response(reader, 200)["db"] == "ok"
                    payload = b'{"body":"ordinary TCP half-close regression"}'
                    # Park only this test's child so complete requests and EOF are ready in the same pass.
                    os.killpg(app.pid, signal.SIGSTOP)
                    try:
                        _, state = os.waitpid(app.pid, os.WUNTRACED)
                        assert os.WIFSTOPPED(state)
                        peer.sendall(request("/posts", token=token, body=payload)
                                     + request("/posts/1") + request("/health"))
                        peer.shutdown(socket.SHUT_WR)
                    finally:
                        os.killpg(app.pid, signal.SIGCONT)
                    post = response(reader, 201)["post"]
                    assert post["body"] == json.loads(payload)["body"]
                    assert response(reader, 200)["post"]["id"] == 1
                    assert response(reader, 200)["db"] == "ok"
                    assert reader.read(1) == b"", "expected peer EOF while the older connection stays open"
                older.sendall(request(f"/posts/{post['id']}"))
                assert response(old_reader, 200)["post"]["body"] == post["body"]
        finally:
            if app.poll() is None:
                os.killpg(app.pid, signal.SIGTERM)
                try:
                    app.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(app.pid, signal.SIGKILL)
                    app.wait()
        with sqlite3.connect(db) as sql:
            assert sql.execute("SELECT body FROM posts WHERE id=?", (post["id"],)).fetchone() == (post["body"],)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("server", "seed_db", "tokens_json"):
        parser.add_argument(key, type=Path)
    args = parser.parse_args()
    check(args.server.resolve(strict=True), args.seed_db, args.tokens_json)
    print("Ordered replies, TCP EOF, persisted post and older connection verified.")
