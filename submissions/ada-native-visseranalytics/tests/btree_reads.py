#!/usr/bin/env python3
"""Compare the B-tree read path (GET /feed, GET /posts/:id) byte for byte with the
reference SQL, on a generated database, under a random mix of likes and new posts
sent through the server, plus rows the fast path must hand back to SQL."""
import base64
import hashlib
import hmac
import http.client
import json
import os
from pathlib import Path
import random
import sqlite3
import subprocess
import tempfile
import time

ADA = Path(__file__).resolve().parents[1]
ROOT = ADA.parents[1]
SECRET = "btree-read-test"
ROW = ("SELECT p.id,p.body,p.created_at,u.username,"
       "(SELECT count(*) FROM likes l WHERE l.post_id=p.id) "
       "FROM posts p JOIN users u ON u.id=p.user_id")
POST = ROW + " WHERE p.id=?"
FEED = ROW + " ORDER BY p.created_at DESC,p.id DESC LIMIT 20"
FIELDS = ("id", "body", "created_at", "author", "like_count")


def wire(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode()


def token(user_id, username):
    def part(value):
        return base64.urlsafe_b64encode(json.dumps(value, separators=(",", ":")).encode()).rstrip(b"=")
    signed = part({"alg": "HS256", "typ": "JWT"}) + b"." + part(
        {"sub": str(user_id), "username": username, "exp": int(time.time()) + 3600})
    signature = base64.urlsafe_b64encode(hmac.new(SECRET.encode(), signed, hashlib.sha256).digest())
    return (signed + b"." + signature.rstrip(b"=")).decode()


with tempfile.TemporaryDirectory(prefix="ada-btree-reads-") as temporary:
    db = Path(temporary) / "feed.db"
    connection = sqlite3.connect(db)
    connection.executescript((ROOT / "schema.sql").read_text())
    rng = random.Random(20261030)
    names = [f"user-{i}" if i % 7 else f"ünï \"{i}\" \\ ✓" for i in range(1, 401)]
    connection.executemany("INSERT INTO users(id,username) VALUES(?,?)", list(enumerate(names, 1)))
    bodies = ["plain", "quote\" slash\\ newline\n tab\t NUL\0 🚀", "x" * 500, "ünïcødé ✓", "\x01\x1f"]
    created = ["plain", "quote\" slash\\ newline\n tab\t 🚀", "x" * 500, "ünïcødé ✓"]
    for post_id in range(1, 3001):
        connection.execute("INSERT INTO posts(id,user_id,body,created_at) VALUES(?,?,?,?)",
                           (post_id, rng.randint(1, 400), rng.choice(bodies),
                            f"2026-01-01T00:{post_id // 60 % 60:02d}:{post_id % 60:02d}.000Z"))
    connection.executemany("INSERT OR IGNORE INTO likes(user_id,post_id) VALUES(?,?)",
                           [(rng.randint(1, 400), rng.randint(1, 3000)) for _ in range(12000)])
    # Rows the fast path must leave to SQL: an author that does not exist, and a
    # user_id stored as text (the join finds no author for either).
    connection.execute("INSERT INTO posts(id,user_id,body,created_at) VALUES(3001,9999,'orphan',"
                       "'2025-01-01T00:00:00.000Z')")
    connection.execute("PRAGMA foreign_keys=OFF")
    connection.execute("INSERT INTO posts(id,user_id,body,created_at) VALUES(3002,'abc','text user',"
                       "'2025-01-01T00:00:00.000Z')")
    connection.commit()
    env = os.environ | dict(SQLITE_PATH=str(db), JWT_SECRET=SECRET, HOST="127.0.0.1", PORT="3004")
    with (Path(temporary) / "server.log").open("w+") as log:
        process = subprocess.Popen(["bash", str(ADA / "start.sh")], env=env, stdout=log, stderr=log)
        client = None
        try:
            deadline = time.monotonic() + 15
            while True:
                assert process.poll() is None, "server exited"
                try:
                    client = http.client.HTTPConnection("127.0.0.1", 3004, timeout=10)
                    client.request("GET", "/health")
                    client.getresponse().read()
                    break
                except OSError:
                    assert time.monotonic() < deadline
                    time.sleep(.05)

            def call(method, path, body=None, headers=None):
                client.request(method, path, body=body, headers=headers or {})
                response = client.getresponse()
                return response.status, response.read()

            def check_post(post_id, label):
                status, body = call("GET", f"/posts/{post_id}")
                row = connection.execute(POST, (post_id,)).fetchone()
                if row is None:
                    assert status == 404, (label, post_id, status, body)
                else:
                    assert (status, body) == (200, wire({"post": dict(zip(FIELDS, row))})), \
                        (label, post_id, status, body, row)

            def check_feed(label):
                status, body = call("GET", "/feed")
                expected = {"posts": [dict(zip(FIELDS, row)) for row in connection.execute(FEED)]}
                assert (status, body) == (200, wire(expected)), (label, status, body[:400])

            for post_id in (1, 2, 1500, 2999, 3000, 3001, 3002, 3003, 10**6, 2**63 - 1):
                check_post(post_id, "fixed")
            check_feed("seeded")
            print("PASS: fixed rows (orphan and text-typed author fall back to SQL)", flush=True)

            top = 3002
            counts = {"feed": 0, "post": 0, "like": 0, "create": 0}
            for step in range(6000):
                user_id = rng.randint(1, 400)
                auth = {"Authorization": "Bearer " + token(user_id, names[user_id - 1])}
                roll = rng.random()
                if roll < 0.15:
                    post_id = rng.choice([rng.randint(1, top), top - rng.randint(0, 25)])
                    status, body = call("POST", f"/posts/{post_id}/like", headers=auth)
                    assert status in (200, 201, 404), (status, body)
                    counts["like"] += 1
                elif roll < 0.20:
                    status, body = call("POST", "/posts", body=json.dumps({"body": rng.choice(created)}),
                                        headers=auth | {"Content-Type": "application/json"})
                    assert status == 201, (status, body)
                    top = max(top, json.loads(body)["post"]["id"])
                    counts["create"] += 1
                elif roll < 0.55:
                    check_feed(f"step {step}")
                    counts["feed"] += 1
                else:
                    check_post(rng.choice([rng.randint(1, top), top - rng.randint(0, 25), top + 1]),
                               f"step {step}")
                    counts["post"] += 1
            print("PASS: random mix", counts, flush=True)
        except BaseException:
            log.seek(0)
            print(log.read()[-4000:], flush=True)
            raise
        finally:
            if client is not None:
                client.close()
            process.terminate()
            process.wait(timeout=10)
            connection.close()
