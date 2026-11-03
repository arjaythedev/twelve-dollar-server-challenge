#!/usr/bin/env python3
"""Compare adaptive feed scans with the reference SQL on adversarial live data."""
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
QUERY = ("SELECT p.id,p.body,p.created_at,u.username,"
         "(SELECT count(*) FROM likes l WHERE l.post_id=p.id) "
         "FROM posts p JOIN users u ON u.id=p.user_id "
         "ORDER BY p.created_at DESC,p.id DESC LIMIT 20")

with tempfile.TemporaryDirectory(prefix="ada-feed-ranges-") as temporary:
    db = Path(temporary) / "feed.db"
    connection = sqlite3.connect(db)
    connection.executescript((ROOT / "schema.sql").read_text())
    connection.executemany("INSERT INTO users(id,username) VALUES(?,?)",
                           [(i, f"author-{i}") for i in range(1, 51)])
    connection.commit()
    env = os.environ | dict(SQLITE_PATH=str(db), JWT_SECRET="feed-range-test",
                            HOST="127.0.0.1", PORT="3003")
    with (Path(temporary) / "server.log").open("w+") as log:
        process = subprocess.Popen(["bash", str(ADA / "start.sh")], env=env,
                                   stdout=log, stderr=log)
        try:
            def get(path):
                client = http.client.HTTPConnection("127.0.0.1", 3003, timeout=10)
                try:
                    client.request("GET", path)
                    response = client.getresponse()
                    assert response.status == 200, response.read()
                    return response.read()
                finally:
                    client.close()
            deadline = time.monotonic() + 15
            while True:
                assert process.poll() is None, "server exited"
                try:
                    get("/health")
                    break
                except OSError:
                    assert time.monotonic() < deadline
                    time.sleep(.05)

            def compare(label):
                rows = connection.execute(QUERY).fetchall()
                expected = {"posts": [dict(zip(("id", "body", "created_at", "author", "like_count"), row))
                                      for row in rows]}
                wire = json.dumps(expected, ensure_ascii=False, separators=(",", ":")).encode()
                assert get("/feed") == wire, label
                print("PASS:", label, flush=True)

            cases = [([], False), (list(range(1, 39)), False),
                     (list(range(10, 1000, 31)), False),
                     (list(range(2**63 - 30, 2**63)), False),
                     (list(range(-2**63, -2**63 + 30)), False),
                     ([-2**63, 0, 2**63 - 1], False),
                     (list(range(1, 39)), True)]
            for trial, (ids, orphan) in enumerate(cases):
                connection.execute("DELETE FROM likes")
                connection.execute("DELETE FROM posts")
                for i, post_id in enumerate(ids):
                    stamp = f"2026-01-01T00:00:00.{(i * 17) % 39:03d}Z"
                    connection.execute("INSERT INTO posts(id,user_id,body,created_at) VALUES(?,?,?,?)",
                                       (post_id, 999 if orphan and i == 16 else i % 50 + 1,
                                        f"row {i}: quote\" slash\\ newline\n NUL\0 🚀", stamp))
                    for user in range(1, i % 4 + 1):
                        connection.execute("INSERT INTO likes(user_id,post_id) VALUES(?,?)", (user, post_id))
                connection.commit()
                compare(f"case {trial}: empty/dense/sparse/extreme/orphan data")
                if ids:
                    connection.execute("INSERT INTO likes(user_id,post_id) VALUES(50,?)", (ids[-1],))
                    connection.commit()
                    compare(f"case {trial}: externally committed like is live")
            random.seed(98125)
            for trial in range(20):
                connection.execute("UPDATE posts SET created_at=? WHERE id=?",
                                   (f"2026-02-01T00:00:00.{random.randrange(1000):03d}Z", random.choice(ids)))
                connection.commit()
                compare(f"timestamp reorder {trial}")
        except BaseException:
            log.seek(0)
            print(log.read(), flush=True)
            raise
        finally:
            process.terminate()
            process.wait(timeout=10)
            connection.close()
