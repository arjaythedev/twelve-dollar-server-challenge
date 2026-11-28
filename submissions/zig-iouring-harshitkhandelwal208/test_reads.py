"""Compare HTTP reads with SQLite on small pages, dense indexes and large JSON responses."""

import base64
from contextlib import closing
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

HERE = Path(__file__).resolve().parent
SELECT = """
    SELECT p.id, p.body, p.created_at, u.username AS author,
           (SELECT count(*) FROM likes WHERE post_id = p.id) AS like_count
      FROM posts p JOIN users u ON u.id = p.user_id
"""


def fixture_token():
    def b64(raw):
        return base64.urlsafe_b64encode(raw).rstrip(b"=")

    header = b64(b'{"alg":"HS256","typ":"JWT"}')
    claims = {"sub": "1", "username": "chunk-test", "exp": int(time.time()) + 60}
    signed = header + b"." + b64(json.dumps(claims).encode())
    return signed + b"." + b64(hmac.digest(b"fixture-secret", signed, "sha256"))


def check_reads(path, sparse):
    def user_id(i):
        if not sparse or i == 1:
            return i
        # Scatter authors across leaves and put a gap above the 32-bit range.
        slot = ((i - 2) * 137) % 1799
        return 100 + slot * 3 + ((1 << 40) if slot > 1700 else 0)

    with closing(sqlite3.connect(path)) as db, db:
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA page_size = 512" if sparse else "PRAGMA page_size = 1024")
        db.executescript((HERE.parent.parent / "schema.sql").read_text())
        db.executemany("INSERT INTO users(id,username) VALUES (?,?)",
                       ((user_id(i), f"{i}-" + "名" * 1100) for i in range(1, 27)))
        db.executemany("INSERT INTO users(id,username) VALUES (?,?)",
                       ((user_id(i), str(i)) for i in range(27, 1801)))
        ids = [i * (1000 if sparse else 1) for i in range(1, 27)]
        for i, pid in enumerate(ids):
            # Ties and creation times deliberately differ from rowid order.
            date = f"2026-01-{1 + (i * 7) % 13:02}T00:00:00.000Z"
            if sparse and i == 25:
                # A fractional timestamp can overflow a small index page even
                # when the record length still fits in a single varint byte.
                date = "2026-02-01T00:00:00." + "0" * 91 + "Z"
            body = ("名" * 497 + f"{i:03}") if i % 2 else ("\t" * 497 + f"{i:03}")
            db.execute("INSERT INTO posts VALUES (?,?,?,?)", (pid, user_id(i + 1), body, date))
            likes = 1500 if i in (8, 25) else 3 + i
            db.executemany("INSERT INTO likes(user_id,post_id) VALUES (?,?)",
                           ((user_id(u), pid) for u in range(1, likes + 1)))
        expected = [dict(r) for r in db.execute(SELECT + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20")]
        singles = [dict(r) for r in db.execute(SELECT + " ORDER BY p.id")]

    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    env = dict(os.environ, SQLITE_PATH=str(path), HOST="127.0.0.1", PORT=str(port), JWT_SECRET="fixture-secret")
    with subprocess.Popen([HERE / "start.sh"], env=env) as server:
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        try:
            for _ in range(100):
                if server.poll() is not None:
                    raise RuntimeError("server exited during startup")
                try:
                    conn.connect()
                    break
                except ConnectionRefusedError:
                    time.sleep(0.05)
            requests = [("/feed", {"posts": expected})]
            requests.extend((f"/posts/{row['id']}", {"post": row}) for row in singles)
            for endpoint, payload in requests:
                conn.request("GET", endpoint)
                response = conn.getresponse()
                body = response.read()
                assert response.status == 200, (endpoint, response.status)
                assert len(body) == int(response.getheader("Content-Length")), endpoint
                assert json.loads(body) == payload, endpoint

            # http.client frames an iterable body as separate HTTP/1.1 chunks.
            token = fixture_token()
            conn.request("POST", "/posts", iter([b'{"body":', b'"two chunks"}']),
                         headers={"Authorization": "Bearer " + token.decode()}, encode_chunked=True)
            created = conn.getresponse()
            assert created.status == 201 and json.loads(created.read())["post"]["body"] == "two chunks"
            conn.request("GET", "/health")
            health = conn.getresponse()
            assert health.status == 200 and json.loads(health.read())["db"] == "ok"

            # A peer can finish sending before the response has been written.
            with socket.create_connection(("127.0.0.1", port), timeout=5) as peer:
                peer.sendall(b"GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n")
                peer.shutdown(socket.SHUT_WR)
                with http.client.HTTPResponse(peer) as reply:
                    reply.begin()
                    assert reply.status == 200 and json.loads(reply.read())["posts"]
                assert peer.recv(1) == b""
        finally:
            conn.close()
            server.terminate()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as directory:
        for sparse in (False, True):
            check_reads(Path(directory) / f"feed-{sparse}.db", sparse)
    print("Both fixtures passed feed, point-read, chunked keep-alive and half-close checks.")
