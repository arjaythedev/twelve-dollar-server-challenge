"""Additional checks against a running server and its SQLite file."""
import base64
import concurrent.futures
import hashlib
import hmac
import http.client
import json
import os
import socket
import sqlite3
import time

port = int(os.environ.get("PORT", "18374"))


def encode(value):
    return base64.urlsafe_b64encode(json.dumps(value).encode()).rstrip(b"=")


def token(name):
    msg = encode({"alg": "HS256", "typ": "JWT"}) + b"." + encode({
        "sub": "1", "username": name, "exp": int(time.time()) + 3600})
    sig = hmac.new(b"twelve-dollar-challenge", msg, hashlib.sha256).digest()
    return (msg + b"." + base64.urlsafe_b64encode(sig).rstrip(b"=")).decode()


name = 'author " café ✓'
headers = {"Authorization": "Bearer " + token(name), "Content-Type": "application/json"}


def request(method, path, body=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
    conn.request(method, path, body=body, headers=headers)
    response = conn.getresponse()
    raw = response.read()
    try:
        result = response.status, json.loads(raw)
    except ValueError:
        raise AssertionError((response.status, raw))
    conn.close()
    return result


for char in ("é", "✓", "😀"):
    status, data = request("POST", "/posts", json.dumps({"body": "\u2003" + char * 500 + "\u00a0"}))
    assert status == 201, (status, data)
    post = data["post"]
    assert post["body"] == char * 500 and post["author"] == name
    # A separate connection sees the row immediately after the HTTP success.
    with sqlite3.connect(os.environ["SQLITE_PATH"]) as db:
        assert db.execute("SELECT body FROM posts WHERE id=?", (post["id"],)).fetchone()[0] == char * 500
    assert request("POST", "/posts", json.dumps({"body": char * 501}))[0] == 400

# Multiple deferred pipelines in one completion batch exercise both handle queues.
wire_body = json.dumps({"body": "pipeline commit"}).encode()
wire = (f"POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: {headers['Authorization']}\r\n"
        f"Content-Length: {len(wire_body)}\r\nContent-Type: application/json\r\n\r\n").encode() + wire_body


def pipeline(_):
    with socket.create_connection(("127.0.0.1", port), timeout=15) as sock:
        sock.sendall(wire * 12)
        stream = sock.makefile("rb")
        ids = []
        for _ in range(12):
            assert stream.readline().startswith(b"HTTP/1.1 201")
            length = None
            while (line := stream.readline()) != b"\r\n":
                assert line
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":", 1)[1])
            ids.append(json.loads(stream.read(length))["post"]["id"])
        assert ids == sorted(ids) and len(set(ids)) == 12
        return ids


with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
    ids = sum(pool.map(pipeline, range(16)), [])
assert len(set(ids)) == 192
with sqlite3.connect(os.environ["SQLITE_PATH"]) as db:
    assert db.execute("SELECT count(*) FROM posts WHERE body='pipeline commit'").fetchone()[0] == 192
print("Unicode, escaped JWT claims, independent commit visibility, and 192 pipelined writes passed")

# Compare both feed plans with the straightforward SQL, including ties and gaps.
select = """SELECT p.id, p.body, p.created_at, u.username,
 (SELECT count(*) FROM likes l WHERE l.post_id=p.id)
 FROM posts p JOIN users u ON u.id=p.user_id
 ORDER BY p.created_at DESC, p.id DESC LIMIT 20"""
for changes in (
    [("2099-01-01T00:00:00.000Z", 499998), ("2099-01-01T00:00:00.000Z", 499997)],
    [("2100-01-01T00:00:00.000Z", 1)],
):
    with sqlite3.connect(os.environ["SQLITE_PATH"]) as db:
        db.executemany("UPDATE posts SET created_at=? WHERE id=?", changes)
        db.commit()
        expected = [dict(zip(("id", "body", "created_at", "author", "like_count"), row))
                    for row in db.execute(select)]
    assert request("GET", "/feed") == (200, {"posts": expected})
print("Narrow-range feed with ties/gaps and wide-range fallback match live SQLite")
