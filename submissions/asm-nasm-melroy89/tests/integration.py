#!/usr/bin/env python3
"""Public-wire/persistence regressions on a disposable seed, never the user's database."""
import argparse
import base64
import concurrent.futures
import hashlib
import hmac
import http.client
import json
import os
import pathlib
import shutil
import signal
import socket
import sqlite3
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
REPO = ROOT.parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--seed", type=pathlib.Path, default=REPO / "seed/feed.db")
parser.add_argument("--idle", action="store_true", help="also wait 66 seconds to verify original-socket reuse")
parser.add_argument("--server-arg", action="append", default=[])
args = parser.parse_args()
secret = b"integration-test-key"
port = 3189
def token(sub="1", username="integration 😀", **extra):
    encode = lambda x: base64.urlsafe_b64encode(json.dumps(x, separators=(",", ":")).encode()).rstrip(b"=")
    header = encode({"alg": "HS256", "typ": "JWT"})
    payload = encode(dict(sub=sub, username=username, exp=int(time.time()) + 3600) | extra)
    data = header + b"." + payload
    return (data + b"." + base64.urlsafe_b64encode(hmac.digest(secret, data, "sha256")).rstrip(b"=")).decode()

def request(method, path, body=None, auth=None, timeout=5):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    headers = {}
    if auth is not None:
        headers["Authorization"] = "Bearer " + auth
    if body is not None:
        if not isinstance(body, bytes):
            body = json.dumps(body, ensure_ascii=False).encode()
        headers["Content-Type"] = "application/json"
    c.request(method, path, body, headers)
    r = c.getresponse()
    result = r.status, json.loads(r.read())
    c.close()
    return result

def response(s):
    data = b""
    while b"\r\n\r\n" not in data:
        part = s.recv(65536)
        assert part, "truncated headers"
        data += part
    headers, body = data.split(b"\r\n\r\n", 1)
    length = int(next(x.split(b":", 1)[1] for x in headers.split(b"\r\n") if x.lower().startswith(b"content-length:")))
    while len(body) < length:
        part = s.recv(65536)
        assert part, "truncated body"
        body += part
    assert len(body) == length
    return int(headers.split()[1]), json.loads(body)

with tempfile.TemporaryDirectory() as tmp:
    db = pathlib.Path(tmp) / "feed.db"
    shutil.copyfile(args.seed, db)
    env = os.environ | dict(SQLITE_PATH=str(db), JWT_SECRET=secret.decode(), HOST="127.0.0.1", PORT=str(port))
    log = open(pathlib.Path(tmp) / "server.log", "w+")
    process = None
    def start():
        p = subprocess.Popen([str(ROOT / "bin/server"), *args.server_arg], env=env, stdout=log, stderr=log)
        for _ in range(100):
            assert p.poll() is None, "server exited"
            try:
                if request("GET", "/health", timeout=.1)[0] == 200:
                    return p
            except OSError:
                pass
            time.sleep(.05)
        p.terminate()
        p.wait(5)
        raise AssertionError("startup timeout")
    try:
        process = start()
        auth = token()
        for body in ["😀" * 500, "café ✓", '\u2003trim me\u3000', 'quoted " slash \\ nul \x00 suffix']:
            status, result = request("POST", "/posts", {"body": body}, auth)
            assert status == 201, (status, result)
            post = result["post"]
            assert post["body"] == body.strip()
            assert request("GET", "/posts/" + str(post["id"]))[1]["post"]["body"] == body.strip()
        assert request("POST", "/posts", {"body": "😀" * 501}, auth)[0] == 400
        assert request("POST", "/posts/abc/like")[0] == 401
        assert request("POST", "/posts/abc/like", auth=auth)[0] == 400
        for kwargs, message in [
            (dict(sub="0"), "invalid token payload"),
            (dict(username=1), "invalid token payload"),
            (dict(exp=int(time.time()) - 1), "invalid or expired token"),
            (dict(nbf=int(time.time()) + 300), "invalid or expired token"),
        ]:
            assert request("POST", "/posts", {"body": "x"}, token(**kwargs)) == (401, {"error": message})
        # Constraint failure must leave the next write usable; ACK must survive SIGKILL.
        assert request("POST", "/posts", {"body": "bad FK"}, token(sub="999999999"))[0] == 500
        status, result = request("POST", "/posts", {"body": "committed before ack"}, auth)
        assert status == 201
        committed_id = result["post"]["id"]
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            results = list(pool.map(lambda _: request("POST", f"/posts/{committed_id}/like", auth=auth), range(32)))
        assert sum(status == 201 for status, _ in results) == 1
        assert sum(status == 200 for status, _ in results) == 31
        assert request("GET", f"/posts/{committed_id}")[1]["post"]["like_count"] == 1
        # Empty JSON-typed like, chunked JSON and split multibyte input.
        data = json.dumps({"body": "fragmented 😀 upload"}, ensure_ascii=False).encode()
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.sendall(f"POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {auth}\r\nContent-Type: application/json\r\nContent-Length: {len(data)}\r\n\r\n".encode())
            split = data.index("😀".encode()) + 2
            s.sendall(data[:split]);time.sleep(.02);s.sendall(data[split:])
            assert response(s)[0] == 201
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.sendall(f"POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {auth}\r\nTransfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n".encode())
            assert s.recv(1024) == b"HTTP/1.1 100 Continue\r\n\r\n"
            chunked = b"".join(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n" for chunk in [data[:11], data[11:]])
            s.sendall(chunked + b"0\r\n\r\n")
            assert response(s)[0] == 201
        # HTTP framing ambiguity must be rejected without processing an embedded request.
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.sendall(b"POST /posts HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n")
            assert response(s)[0] == 400
        process.send_signal(signal.SIGKILL);process.wait(5)
        with sqlite3.connect(db) as conn:
            assert conn.execute("SELECT body FROM posts WHERE id=?", (committed_id,)).fetchone() == ("committed before ack",)
            assert conn.execute("PRAGMA integrity_check").fetchone() == ("ok",)
        conn.close()
        process = start()
        assert request("GET", f"/posts/{committed_id}")[1]["post"]["body"] == "committed before ack"
        if args.idle:
            with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
                s.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
                assert response(s)[0] == 200
                time.sleep(66)
                s.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
                assert response(s)[0] == 200
        print("PASS: auth, Unicode, FK rollback, duplicate likes, fragmented/chunked/continue, framing, crash recovery")
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:process.wait(5)
            except subprocess.TimeoutExpired:process.kill();process.wait()
        log.seek(0)
        print(log.read(), end="")
        log.close()
