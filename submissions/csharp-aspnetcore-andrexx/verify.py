#!/usr/bin/env python3
"""Integration checks against the published app; no third-party test packages."""
import base64
import concurrent.futures
import hashlib
import hmac
import json
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

HERE = Path(__file__).resolve().parent
SECRET = "integration-test-secret"


def token(payload=None, algorithm="HS256", secret=SECRET):
    def encode(value):
        return base64.urlsafe_b64encode(value).rstrip(b"=").decode()
    if payload is None:
        payload = {"sub": "1", "username": "token-name", "exp": time.time() + 3600}
    header = encode(json.dumps({"alg": algorithm}).encode())
    body = encode(json.dumps(payload).encode())
    signature = encode(hmac.new(secret.encode(), f"{header}.{body}".encode(), hashlib.sha256).digest())
    return f"{header}.{body}.{signature}"


def request(base, method, path, payload=None, authorization=None):
    data = payload.encode() if isinstance(payload, str) else None
    if payload is not None and not isinstance(payload, str):
        data = json.dumps(payload, ensure_ascii=False).encode()
    headers = {"Content-Type": "application/json"}
    if authorization is not None:
        headers["Authorization"] = authorization
    req = urllib.request.Request(base + path, data=data, headers=headers, method=method)
    try:
        response = urllib.request.urlopen(req, timeout=15)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        raw = response.read().decode()
        assert response.headers["Content-Type"].startswith("application/json")
        assert "\n" not in raw, raw
        return response.status, json.loads(raw)


def expect(base, method, path, status, body, payload=None, authorization=None):
    actual = request(base, method, path, payload, authorization)
    assert actual == (status, body), (method, path, actual, (status, body))


def run_checks(base, db):
    auth = "Bearer " + token()
    expect(base, "GET", "/unknown", 404, {"error": "not found"})
    for body in ["[]", "null", '"text"', '{"body":null}', '{"body":true}']:
        expect(base, "POST", "/posts", 400, {"error": "body is required"}, body, auth)
    for body in ["", "{", '{"body":"x"} trailing']:
        expect(base, "POST", "/posts", 400, {"error": "malformed JSON body"}, body, auth)
    expect(base, "POST", "/posts/abc/like", 401, {"error": "missing bearer token"}, authorization=None)
    expect(base, "POST", "/posts/abc/like", 400, {"error": "invalid post id"}, authorization=auth)
    expect(base, "POST", "/posts", 401, {"error": "missing bearer token"}, "{", "Basic abc")
    # HTTP strips trailing header whitespace, leaving "Bearer" with no prefix space.
    expect(base, "POST", "/posts", 401, {"error": "missing bearer token"}, "{", "Bearer ")
    invalid_tokens = ["abc", token(algorithm="HS512"), token(secret="wrong"),
                      token({"sub": "1", "username": "x"}),
                      token({"sub": "1", "username": "x", "exp": time.time() - 1}),
                      token({"sub": "1", "username": "x", "exp": "9999999999"}),
                      token([1, 2, 3])]
    for jwt in invalid_tokens:
        expect(base, "POST", "/posts", 401, {"error": "invalid or expired token"}, "{", "Bearer " + jwt)
    for subject, username in [(0, "x"), ("0", "x"), ("-1", "x"), ("1.5", "x"), (" 1", "x"), ("1", None)]:
        jwt = token({"sub": subject, "username": username, "exp": time.time() + 3600})
        expect(base, "POST", "/posts", 401, {"error": "invalid token payload"}, "{", "Bearer " + jwt)

    # All fixture posts share a timestamp; ID must break the tie, limiting to 20.
    status, feed = request(base, "GET", "/feed")
    assert status == 200 and [post["id"] for post in feed["posts"]] == list(range(25, 5, -1))
    body = "😀" * 500
    status, created = request(base, "POST", "/posts", {"body": " \t" + body + "\r\n"}, auth)
    post = created["post"]
    assert status == 201 and post["body"] == body and post["author"] == "token-name"
    assert list(post) == ["id", "body", "created_at", "author", "like_count"]
    with sqlite3.connect(db) as connection:
        # Independent reader sees the committed row immediately.
        assert connection.execute("SELECT body FROM posts WHERE id=?", (post["id"],)).fetchone()[0] == body
        assert connection.execute("PRAGMA journal_mode").fetchone()[0] == "wal"
    expect(base, "POST", "/posts", 400, {"error": "body must be at most 500 characters"}, {"body": body + "😀"}, auth)

    def like(_):
        return request(base, "POST", f'/posts/{post["id"]}/like', authorization=auth)
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        likes = list(pool.map(like, range(32)))
    assert sum(status == 201 for status, _ in likes) == 1
    assert sum(status == 200 for status, _ in likes) == 31
    for status, result in likes:
        assert result == {"liked": True, "already_liked": status == 200, "post_id": post["id"]}
    with sqlite3.connect(db) as connection:
        assert connection.execute("SELECT count(*) FROM likes WHERE post_id=?", (post["id"],)).fetchone()[0] == 1
    status, loaded = request(base, "GET", f'/posts/{post["id"]}')
    assert status == 200 and loaded["post"]["like_count"] == 1
    assert loaded["post"]["author"] == "db-name"  # Reads join users; creation uses token username.
    status, feed = request(base, "GET", "/feed")
    assert status == 200 and feed["posts"][0] == loaded["post"]
    expect(base, "POST", "/posts/9999/like", 404, {"error": "post not found"}, authorization=auth)
    missing_user = "Bearer " + token({"sub": "9999", "username": "missing", "exp": time.time() + 3600})
    expect(base, "POST", "/posts", 500, {"error": "internal server error"}, {"body": "x"}, missing_user)


def check_server(db, log, degraded=False):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    env = dict(os.environ, SQLITE_PATH=str(db), JWT_SECRET=SECRET, HOST="127.0.0.1", PORT=str(port))
    process = subprocess.Popen(["bash", str(HERE / "start.sh")], env=env, stdout=log, stderr=log)
    try:
        for _ in range(100):
            if process.poll() is not None:
                raise AssertionError("Server exited before readiness")
            try:
                status, health = request(base, "GET", "/health")
                break
            except urllib.error.URLError:
                time.sleep(0.1)
        else:
            raise AssertionError("Server did not become ready")
        if degraded:
            assert status == 503 and health["status"] == "degraded" and health["db"] == "unreachable"
            assert isinstance(health["error"], str)
        else:
            assert status == 200 and list(health) == ["status", "db", "uptime_s"]
            assert isinstance(health["uptime_s"], int) and health["uptime_s"] >= 0
            run_checks(base, db)
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    assert shutil.which("dotnet"), "Install the pinned .NET SDK first"
    assert (HERE / "bin/publish/FeedApi.dll").exists(), "Run bash build.sh first"
    with tempfile.TemporaryDirectory() as directory:
        db = Path(directory) / "feed.db"
        with sqlite3.connect(db) as connection:
            connection.executescript((HERE.parents[1] / "schema.sql").read_text())
            connection.execute("INSERT INTO users(id, username) VALUES (1, 'db-name')")
            connection.executemany("INSERT INTO posts(id, user_id, body, created_at) VALUES (?, 1, 'seed', '2025-01-01T00:00:00.000Z')", [(i,) for i in range(1, 26)])
        with tempfile.TemporaryFile(mode="w+") as log:
            try:
                check_server(db, log)
                check_server(Path(directory) / "missing.db", log, degraded=True)
            except Exception:
                log.seek(0)
                print(log.read())
                raise
    print("Additional integration checks passed")


if __name__ == "__main__":
    main()
