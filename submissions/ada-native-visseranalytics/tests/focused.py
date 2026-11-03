#!/usr/bin/env python3
"""Black-box checks on a private seed copy. Python is test tooling only."""
import base64
import concurrent.futures
import hmac
import http.client
import json
import os
import resource
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[1]
SECRET = "focused-ada-secret"
PORT = int(os.environ.get("PORT", "3001"))
passed = 0


def check(condition, label):
    global passed
    assert condition, label
    passed += 1
    print(f"ok {passed}: {label}", flush=True)


def b64(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=")


def sign(payload=None, header=None, payload_bytes=None, header_bytes=None):
    if payload is None:
        payload = {"sub": "1", "username": "token-name", "exp": time.time() + 3600}
    if header is None:
        header = {"alg": "HS256", "typ": "JWT"}
    a = b64(header_bytes if header_bytes is not None else json.dumps(header).encode())
    b = b64(payload_bytes if payload_bytes is not None else json.dumps(payload).encode())
    data = a + b"." + b
    return (data + b"." + b64(hmac.digest(SECRET.encode(), data, "sha256"))).decode()


def request(method, path, payload=None, token=None, raw=None, connection=None):
    conn = connection or http.client.HTTPConnection("127.0.0.1", PORT, timeout=10)
    headers = {}
    if token is not None:
        headers["Authorization"] = "Bearer " + token
    if payload is not None:
        raw = json.dumps(payload, ensure_ascii=False).encode()
    if raw is not None:
        headers["Content-Type"] = "application/json"
    conn.request(method, path, body=raw, headers=headers)
    response = conn.getresponse()
    content = response.read()
    result = response.status, json.loads(content), content
    assert response.getheader("Content-Type").startswith("application/json")
    if connection is None:
        conn.close()
    return result


def response_from(stream):
    first = stream.readline()
    assert first.startswith(b"HTTP/1.1 "), first
    code = int(first.split()[1])
    headers = {}
    while True:
        line = stream.readline()
        if line == b"\r\n":
            break
        assert line, "closed in response headers"
        key, value = line.split(b":", 1)
        headers[key.lower()] = value.strip()
    length = int(headers.get(b"content-length", 0))
    content = stream.read(length)
    assert len(content) == length
    return code, content


def run(db, process):
    token = sign()
    expected = lambda text: {"error": text}

    for method, path in [("GET", "/health"), ("GET", "/feed?unused=1"), ("GET", "/posts/1")]:
        status, _, _ = request(method, path)
        check(status == 200, f"{path} healthy")

    for sub in [0, 1, None, True, "", "0", "-1", "1.5", " 1", "1 ", "1_0", "abc"]:
        bad = sign({"sub": sub, "username": "x", "exp": time.time() + 3600})
        status, data, _ = request("POST", "/posts/abc/like", token=bad)
        check((status, data) == (401, expected("invalid token payload")), f"invalid subject {sub!r}; auth before id")
    for username in [None, 3, True, [], {}]:
        bad = sign({"sub": "1", "username": username, "exp": time.time() + 3600})
        status, data, _ = request("POST", "/posts", {"body": "x"}, bad)
        check((status, data) == (401, expected("invalid token payload")), f"invalid username {username!r}")
    for expiration in [None, True, "9999999999", time.time() - 1]:
        bad = sign({"sub": "1", "username": "x", "exp": expiration})
        status, data, _ = request("POST", "/posts", {"body": "x"}, bad)
        check((status, data) == (401, expected("invalid or expired token")), f"invalid expiry {expiration!r}")
    invalid_tokens = [
        "garbage", token + ".extra", token[:-1] + ("a" if token[-1] != "a" else "b"),
        sign(header={"alg": "none"}), sign(header={"alg": "HS512"}),
        sign(header={"alg": 256}), sign(header_bytes=b'{"alg":"HS256",}'),
        sign(payload_bytes=b'{"sub":"1","username":"x",}'),
        sign(payload_bytes=b'[]'), sign(payload={"sub": "1", "username": "x"}),
        sign(payload={"sub": "1", "username": "x", "exp": time.time() + 3600, "nbf": time.time() + 600}),
    ]
    for i, bad in enumerate(invalid_tokens):
        status, data, _ = request("POST", "/posts", {"body": "x"}, bad)
        check((status, data) == (401, expected("invalid or expired token")), f"invalid token case {i}")

    for raw in [b'', b'{"body":"x",}', b'{"body":"x"} trailing', b'{"body":"\\x"}',
                b'{"body":"\xff"}', b'{"body":"\\uD800"}', b'{"body":01}',
                b'{"body":NaN}', b'{"body":"x","extra":[1,]}']:
        status, data, _ = request("POST", "/posts", token=token, raw=raw)
        check((status, data) == (400, expected("malformed JSON body")), f"malformed JSON {raw!r}")
    for payload in [None, False, 3, "text", [], {}, {"body": {}}, {"body": None},
                    {"body": "\t\r\n\u00a0\u2003\u2028\u3000\ufeff"}]:
        status, data, _ = request("POST", "/posts", token=token, raw=json.dumps(payload).encode())
        check((status, data) == (400, expected("body is required")), f"body validation {payload!r}")

    for text in ["λ" * 500, "😀" * 500, 'quotes " slash / backslash \\ \n tab \t \b \f end',
                 "a\0b", "café ✓"]:
        status, data, raw = request("POST", "/posts", {"body": "\u2003" + text + "\u00a0"}, token)
        check(status == 201 and data["post"]["body"] == text, f"unicode/control roundtrip ({len(text)} codepoints)")
        post = data["post"]
        check(list(post) == ["id", "body", "created_at", "author", "like_count"], "post key order")
        check(post["author"] == "token-name" and post["like_count"] == 0, "create uses token username")
        with sqlite3.connect(db) as reader:
            stored = reader.execute("SELECT body, created_at FROM posts WHERE id=?", (post["id"],)).fetchone()
        check(stored == (text, post["created_at"]), "201 is visible to a separate SQLite connection")
        status, read, _ = request("GET", f"/posts/{post['id']}")
        check(status == 200 and read["post"]["body"] == text, "database preserves text including NUL")
        check(read["post"]["author"] == "golden_ember_1", "GET joins stored author's username")
    # Feed aggregation must preserve exact escaping and field order, including
    # the Unicode/control/NUL values inserted above, and read current rows.
    with sqlite3.connect(db) as reader:
        rows = reader.execute("""
            SELECT p.id,p.body,p.created_at,u.username,
              (SELECT count(*) FROM likes l WHERE l.post_id=p.id)
            FROM posts p JOIN users u ON u.id=p.user_id
            ORDER BY p.created_at DESC,p.id DESC LIMIT 20
        """).fetchall()
    expected_feed = {"posts": [dict(zip(
        ("id", "body", "created_at", "author", "like_count"), row)) for row in rows]}
    status, _, raw = request("GET", "/feed")
    check(status == 200 and raw == json.dumps(expected_feed, ensure_ascii=False, separators=(",", ":")).encode(),
          "feed bytes match SQLite rows, Unicode/control/NUL escaping and key order")
    status, data, _ = request("POST", "/posts", {"body": "😀" * 501}, token)
    check((status, data) == (400, expected("body must be at most 500 characters")), "501 Unicode characters rejected")

    deep = b'{"body":"nested","extra":' + b"[" * 6000 + b"0" + b"]" * 6000 + b"}"
    status, data, _ = request("POST", "/posts", token=token, raw=deep)
    check(status == 201 and data["post"]["body"] == "nested", "deep valid JSON uses bounded iterative parser")
    status, data, _ = request("POST", "/posts", token=token, raw=b'{"body":false,"body":"last"}')
    check(status == 201 and data["post"]["body"] == "last", "duplicate JSON keys use final value")
    status, data, _ = request("POST", "/posts", {"body": "empty name"}, sign({"sub": "1", "username": "", "exp": time.time() + 3600}))
    check(status == 201 and data["post"]["author"] == "", "empty string username accepted as-is")

    nonuser = sign({"sub": "999999999", "username": "absent", "exp": time.time() + 3600})
    status, data, _ = request("POST", "/posts", {"body": "foreign key"}, nonuser)
    check((status, data) == (500, expected("internal server error")), "foreign-key violation rolls back")
    status, data, _ = request("POST", "/posts", {"body": "concurrent likes"}, token)
    check(status == 201, "server recovers after SQLite error")
    post_id = data["post"]["id"]
    with concurrent.futures.ThreadPoolExecutor(max_workers=24) as pool:
        results = list(pool.map(lambda _: request("POST", f"/posts/{post_id}/like", token=token), range(48)))
    check(sum(s == 201 for s, _, _ in results) == 1, "one concurrent duplicate like creates a row")
    check(sum(s == 200 and d["already_liked"] for s, d, _ in results) == 47, "47 concurrent duplicate likes are idempotent")
    with sqlite3.connect(db) as reader:
        count = reader.execute("SELECT count(*) FROM likes WHERE post_id=?", (post_id,)).fetchone()[0]
        mode = reader.execute("PRAGMA journal_mode").fetchone()[0]
    check(count == 1 and mode == "wal", "like committed exactly once, database remains WAL")
    check(request("GET", f"/posts/{post_id}")[1]["post"]["like_count"] == 1, "fresh like count")
    check(request("GET", "/feed")[1]["posts"][0]["id"] == post_id, "newest post ordering")
    check(request("GET", "/feed")[1]["posts"][0]["like_count"] == 1, "feed reads committed like count")

    # Keep a single TCP connection across requests and verify socket identity.
    conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=80)
    request("GET", "/health", connection=conn)
    original = conn.sock
    for _ in range(30):
        check(request("GET", "/health", connection=conn)[0] == 200 and conn.sock is original,
              "HTTP/1.1 connection reused")
    conn.close()
    with socket.create_connection(("127.0.0.1", PORT), 10) as sock:
        stream = sock.makefile("rb")
        sock.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n" * 12)
        check(all(response_from(stream)[0] == 200 for _ in range(12)), "12 pipelined requests preserve response order")
        stream.close()
    with socket.create_connection(("127.0.0.1", PORT), 10) as sock:
        stream = sock.makefile("rb")
        headers = f"POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {token}\r\nTransfer-Encoding: chunked\r\n\r\n".encode()
        sock.sendall(headers + b'5\r\n{"bod\r\n')
        time.sleep(0.02)
        sock.sendall(b'c\r\ny":"chunk"}\n\r\n0\r\nX-Test: yes\r\n\r\n')
        code, content = response_from(stream)
        check(code == 201 and json.loads(content)["post"]["body"] == "chunk", "fragmented chunked request and trailers")
        stream.close()
    with socket.create_connection(("127.0.0.1", PORT), 10) as sock:
        stream = sock.makefile("rb")
        payload = b'{"body":"continue"}'
        sock.sendall(f"POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {token}\r\nContent-Length: {len(payload)}\r\nExpect: 100-continue\r\n\r\n".encode())
        check(response_from(stream)[0] == 100, "100 Continue without deadlock")
        sock.sendall(payload)
        check(response_from(stream)[0] == 201, "body accepted after 100 Continue")
        stream.close()
    for extra in [b"Content-Length: 0\r\nContent-Length: 0\r\n",
                  b"Transfer-Encoding: chunked\r\nContent-Length: 0\r\n"]:
        with socket.create_connection(("127.0.0.1", PORT), 10) as sock:
            stream = sock.makefile("rb")
            sock.sendall(b"POST /posts HTTP/1.1\r\nHost: localhost\r\n" + extra + b"\r\n")
            check(response_from(stream)[0] == 400, "ambiguous HTTP framing rejected")
            stream.close()

    # Exercise the direct-server limits beyond the old 1,024 socket ceiling.
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (min(hard, max(soft, 4096)), hard))
    clients = []
    try:
        for _ in range(2048):
            c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=10)
            assert request("GET", "/health", connection=c)[0] == 200
            clients.append((c, c.sock))
        check(all(request("GET", "/health", connection=c)[0] == 200 and c.sock is s
                  for c, s in clients), "2,048 simultaneous keep-alive sockets stay usable")
        # Reuse slots repeatedly while older peers retain their identities.
        for c, _ in clients[::2]:
            c.close()
        with concurrent.futures.ThreadPoolExecutor(max_workers=32) as pool:
            check(all(s == 200 for s in pool.map(
                lambda _: request("GET", "/health")[0], range(512))),
                "connection churn preserves request routing")
        check(all(request("GET", "/health", connection=c)[0] == 200 and c.sock is s
                  for c, s in clients[1::2]), "idle peers survive slot reuse")
    finally:
        for c, _ in clients:
            c.close()
    with socket.create_connection(("127.0.0.1", PORT), 10) as sock:
        stream = sock.makefile("rb")
        sock.sendall(b"GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n" * 1024)
        time.sleep(0.1)  # Delay reading to exercise response backpressure.
        check(request("GET", "/health")[0] == 200,
              "large pipeline does not starve a separate client")
        check(all(response_from(stream)[0] == 200 for _ in range(1024)),
              "1,024 pipelined feeds drain through bounded continuations")
        stream.close()

    sockets = []
    try:
        for _ in range(128):
            c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=80)
            request("GET", "/health", connection=c)
            sockets.append((c, c.sock))
        proc_status = Path(f"/proc/{process.pid}/status")
        if proc_status.exists():
            print("Diagnostic memory with 128 idle upstreams:",
                  "; ".join(line for line in proc_status.read_text().splitlines()
                            if line.startswith(("VmRSS:", "VmSize:", "Threads:"))), flush=True)
        print("Checking 66 seconds of idle keep-alive...", flush=True)
        time.sleep(66)
        check(all(request("GET", "/health", connection=c)[0] == 200 and c.sock is s for c, s in sockets),
              "128 connections survive 66 idle seconds (same TCP sockets)")
    finally:
        for c, _ in sockets:
            c.close()


def main():
    seed = ROOT / "seed/feed.db"
    assert seed.is_file(), "Generate seed with bash seed/make-seed.sh first"
    with tempfile.TemporaryDirectory(prefix="ada-focused-") as temp:
        db = Path(temp) / "feed.db"
        shutil.copyfile(seed, db)
        env = os.environ | {"SQLITE_PATH": str(db), "JWT_SECRET": SECRET,
                            "HOST": "127.0.0.1", "PORT": str(PORT), "TZ": "Pacific/Auckland"}
        with open(Path(temp) / "server.log", "w+") as log:
            process = subprocess.Popen([str(HERE / "start.sh")], env=env, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError("server exited before readiness")
                    try:
                        if request("GET", "/health")[0] == 200:
                            break
                    except OSError:
                        time.sleep(0.1)
                else:
                    raise TimeoutError("server not ready within 60 seconds")
                run(db, process)
                # Stop only this script's server and verify durable rows after restart.
                process.terminate()
                process.wait(timeout=10)
                with sqlite3.connect(db) as reader:
                    check(reader.execute("SELECT count(*) FROM posts WHERE body='concurrent likes'").fetchone()[0] == 1,
                          "committed writes survive process termination")
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read(), flush=True)
                raise
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
    print(f"focused passed={passed} failed=0", flush=True)


if __name__ == "__main__":
    main()
