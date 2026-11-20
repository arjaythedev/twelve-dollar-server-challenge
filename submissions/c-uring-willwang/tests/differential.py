#!/usr/bin/env python3
"""Compare HTTP reads with the reference SQL on fresh databases; kill and recover a writer."""

import argparse
import base64
import hashlib
import hmac
import http.client
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import time
from contextlib import closing, contextmanager

SUBMISSION = Path(__file__).resolve().parents[1]
SCHEMA = (SUBMISSION.parents[1] / "schema.sql").read_text()
SECRET = "differential-test-secret"
AUTHOR = 'Zoë / "user" 🤖'
LONG_BODY = ("🧪\n\t\\" * 125)
SELECT = """
    SELECT p.id, p.body, p.created_at, u.username AS author,
           (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
      FROM posts p JOIN users u ON p.user_id = u.id
"""
CASES = ("empty", "short", "dense", "no-likes", "no-recent-likes", "partial", "disorder",
         "time-ties", "sparse", "sparse-ordered", "small-pages", "overflow", "orphan")


def stamp(order):
    return f"2020-01-01T00:{order // 60:02d}:{order % 60:02d}.000Z"


def seed(db_file, case):
    count = {"empty": 0, "short": 7}.get(case, 192)
    rows = []
    for i in range(1, count + 1):
        post_id = i
        order = i
        author = 1 + i % 53
        if case == "partial" and i == 3:
            order = count - 6
        if case == "disorder" and i == count - 4:
            order = count
        if case == "time-ties":
            order = i % 4
        if case in ("sparse", "sparse-ordered"):
            post_id = (1 << 36) + i * 521
            if case == "sparse":
                order = (i * 41) % count
        if case == "orphan" and i > count - 5:
            author = 99999
        body = LONG_BODY if case == "overflow" and i % 6 == 0 else f'post {post_id} "<>&/" café \x01'
        rows.append((post_id, author, body, stamp(order)))

    with closing(sqlite3.connect(db_file)) as db:
        # Exercise both a 20th key on one leaf and one reached through an interior page.
        if case in ("small-pages", "overflow"):
            db.execute("PRAGMA page_size=512")
        db.executescript(SCHEMA)
        db.execute("PRAGMA journal_mode=WAL")
        users = [(i, AUTHOR if i == 1 else f'user-{i} / é') for i in range(1, 230)]
        db.executemany("INSERT INTO users (id,username) VALUES (?,?)", users)
        db.executemany("INSERT INTO posts (id,user_id,body,created_at) VALUES (?,?,?,?)", rows)
        likes = []
        for i, (post_id, _, _, _) in enumerate(rows):
            # A fan-out crossing many leaves, sparse matches, empty ranges and an unliked suffix.
            if case == "no-likes" or (case == "no-recent-likes" and i >= count - 30):
                continue
            n = 202 if i == count - 3 else [0, 1, 0, 4, 13][i % 5]
            likes.extend((user, post_id) for user in range(2, 2 + n))
        db.executemany("INSERT INTO likes (user_id,post_id) VALUES (?,?)", reversed(likes))
        db.commit()


def load_reference(db_file):
    with closing(sqlite3.connect(db_file)) as db:
        db.row_factory = sqlite3.Row
        feed = [dict(row) for row in db.execute(SELECT + " ORDER BY p.created_at DESC,p.id DESC LIMIT 20")]
        posts = {row["id"]: dict(row) for row in db.execute(SELECT)}
        sample = [row[0] for row in db.execute("SELECT id FROM posts ORDER BY id")]
        return feed, posts, sample


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def equal(actual, expected, where):
    # Re-encode to permit equivalent string escaping while preserving key order and numeric types.
    if encoded(actual) != encoded(expected):
        raise AssertionError(f"{where}: expected {str(expected)[:700]}, got {str(actual)[:700]}")


def request(conn, method, path, status=200, body=None, headers=None):
    conn.request(method, path, body=body, headers=headers or {})
    resp = conn.getresponse()
    raw = resp.read()
    if resp.status != status:
        raise AssertionError(f"{method} {path}: expected {status}, got {resp.status}: {raw[:180]!r}")
    if resp.getheader("Content-Type", "").split(";")[0].strip().lower() != "application/json":
        raise AssertionError(f"{method} {path}: expected JSON content type")
    return json.loads(raw)


@contextmanager
def running(server, db_file):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    env = dict(os.environ, HOST="127.0.0.1", PORT=str(port), JWT_SECRET=SECRET, SQLITE_PATH=str(db_file))
    with tempfile.TemporaryFile() as log:
        proc = subprocess.Popen([server], env=env, stdout=log, stderr=log)
        conn = None
        try:
            until = time.monotonic() + 60
            while proc.poll() is None and time.monotonic() < until:
                conn = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
                try:
                    health = request(conn, "GET", "/health")
                    assert health["status"] == "ok" and health["db"] == "ok"
                    break
                except OSError:
                    conn.close()
                    time.sleep(0.02)
            else:
                log.seek(0)
                raise RuntimeError(f"{server} did not start: {log.read()[-1500:].decode(errors='replace')}")
            yield conn
        finally:
            if proc.poll() is None:
                proc.kill()  # Simulate a crash without giving SQLite a graceful shutdown.
            if conn is not None:
                conn.close()
            proc.wait()


def check_reads(conn, reference):
    feed, posts, sample = reference
    equal(request(conn, "GET", "/feed"), {"posts": feed}, "/feed")
    ids = {p["id"] for p in feed} | set(sample[::13]) | set(sample[-5:]) | {42_424_242}
    for post_id in sorted(ids):
        path = f"/posts/{post_id}"
        if post_id in posts:
            equal(request(conn, "GET", path), {"post": posts[post_id]}, path)
        else:
            equal(request(conn, "GET", path, 404), {"error": "post not found"}, path)


def check_pipeline(conn, feed):
    # Send a split request header and body, then two requests in the same write.
    header = (f"POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {token()}\r\n"
              "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n").encode()
    tail = b'xGET /feed HTTP/1.1\r\nHost: localhost\r\n\r\nGET /absent HTTP/1.1\r\nHost: localhost\r\n\r\n'
    with socket.create_connection((conn.host, conn.port), timeout=5) as sock, sock.makefile("rb") as stream:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        for part in (header[:17], header[17:] + b"{"):
            sock.sendall(part)
            time.sleep(0.01)
        sock.sendall(tail)
        for status, obj in [(400, {"error": "malformed JSON body"}), (200, {"posts": feed}),
                            (404, {"error": "not found"})]:
            line = stream.readline()
            if not line.startswith(f"HTTP/1.1 {status} ".encode()):
                raise AssertionError(f"pipelined response expected {status}, got {line!r}")
            headers = http.client.parse_headers(stream)
            raw = stream.read(int(headers["Content-Length"]))
            equal(json.loads(raw), obj, "pipelined response")


def token():
    def b64(data):
        return base64.urlsafe_b64encode(data).rstrip(b"=")

    payload = {"sub": "1", "username": AUTHOR, "exp": int(time.time()) + 3600}
    signed = b64(b'{"alg":"HS256"}') + b"." + b64(encoded(payload).encode())
    return (signed + b"." + b64(hmac.digest(SECRET.encode(), signed, hashlib.sha256))).decode()


def write_and_check(conn, previous_feed):
    headers = {"Authorization": "Bearer " + token(), "Content-Type": "application/json"}
    payload = encoded({"body": "  " + LONG_BODY + "  "}).encode()
    created = request(conn, "POST", "/posts", 201, body=payload, headers=headers)["post"]
    post_id = created["id"]
    expected = {"id": post_id, "body": LONG_BODY.strip(), "created_at": created["created_at"],
                "author": AUTHOR, "like_count": 0}
    equal(created, expected, "POST /posts")
    for status, repeat in ((201, False), (200, True)):
        result = request(conn, "POST", f"/posts/{post_id}/like", status, headers=headers)
        equal(result, {"liked": True, "already_liked": repeat, "post_id": post_id}, "like")
    expected["like_count"] = 1
    equal(request(conn, "GET", f"/posts/{post_id}"), {"post": expected}, "like visible")
    equal(request(conn, "GET", "/feed"), {"posts": [expected, *previous_feed[:19]]}, "feed after write")
    # This ACK is the last interaction before SIGKILL. The next read is made by a new process.
    last = request(conn, "POST", "/posts", 201, body=b'{"body":"last ack before crash"}', headers=headers)
    equal(last["post"]["body"], "last ack before crash", "last acknowledged create")
    return [expected, last["post"]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, default=SUBMISSION / "start.sh",
                        help="foreground server executable (default: this submission's start.sh)")
    parser.add_argument("--case", action="append", choices=CASES, help="run only selected layouts")
    args = parser.parse_args()
    for case in args.case or CASES:
        with tempfile.TemporaryDirectory() as tmp:
            db_file = Path(tmp) / "feed.db"
            seed(db_file, case)
            reference = load_reference(db_file)
            with running(args.server.resolve(), db_file) as conn:
                check_reads(conn, reference)
                if case == "short":
                    check_pipeline(conn, reference[0])
                if case == "dense":
                    committed = write_and_check(conn, reference[0])
            if case == "dense":
                recovered = load_reference(db_file)
                for acknowledged in committed:
                    equal(recovered[1].get(acknowledged["id"]), acknowledged, "SQLite crash recovery")
                with running(args.server.resolve(), db_file) as conn:
                    check_reads(conn, recovered)
            print(f"ok {case}", flush=True)


if __name__ == "__main__":
    main()
