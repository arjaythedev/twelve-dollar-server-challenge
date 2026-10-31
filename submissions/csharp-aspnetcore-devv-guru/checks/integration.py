#!/usr/bin/env python3
"""Additional black-box checks. Standard library only; never changes the seed."""
import argparse
import base64
import concurrent.futures
from contextlib import closing
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


def token(secret, payload=None, algorithm="HS256"):
    if payload is None:
        payload = {"sub": "1", "username": "golden_ember_1", "exp": int(time.time()) + 3600}
    encode = lambda value: base64.urlsafe_b64encode(value).rstrip(b"=")
    header = encode(json.dumps({"alg": algorithm, "typ": "JWT"}).encode())
    body = encode(json.dumps(payload, ensure_ascii=False).encode())
    data = header + b"." + body
    signature = encode(hmac.new(secret.encode(), data, hashlib.sha256).digest())
    return (data + b"." + signature).decode()


class Server:
    def __init__(self, executable, database, secret, log, prefix=None):
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]
        self.executable, self.database, self.secret = executable, database, secret
        self.log, self.prefix, self.process = log, prefix or [], None

    def start(self):
        env = dict(os.environ, SQLITE_PATH=str(self.database), JWT_SECRET=self.secret,
                   HOST="127.0.0.1", PORT=str(self.port))
        self.process = subprocess.Popen(self.prefix + [str(self.executable)], env=env,
                                        stdout=self.log, stderr=subprocess.STDOUT,
                                        cwd=Path(self.executable.anchor))
        for _ in range(120):
            if self.process.poll() is not None:
                raise AssertionError("server exited during startup")
            try:
                if self.request("GET", "/health")[0] == 200:
                    return
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(0.1)
        raise AssertionError("server did not become healthy")

    def stop(self, kill=False):
        if self.process is not None and self.process.poll() is None:
            self.process.kill() if kill else self.process.terminate()
            self.process.wait(timeout=15)

    def request(self, method, path, bearer=None, body=None, raw=None):
        headers = {}
        if bearer is not None:
            headers["Authorization"] = "Bearer " + bearer
        if body is not None:
            raw = json.dumps(body, ensure_ascii=False).encode()
        if raw is not None:
            headers["Content-Type"] = "application/json"
        request = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}",
                                         method=method, headers=headers, data=raw)
        try:
            response = urllib.request.urlopen(request, timeout=15)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            data = response.read()
            return response.status, json.loads(data), data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=Path, required=True)
    parser.add_argument("--server", type=Path, required=True)
    args = parser.parse_args()
    passed = 0

    def check(condition, label):
        nonlocal passed
        assert condition, label
        passed += 1
        print("ok  " + label, flush=True)

    with tempfile.TemporaryDirectory(prefix="feed-api-checks-") as scratch:
        database = Path(scratch) / "feed.db"
        shutil.copyfile(args.seed, database)
        log_path = Path(scratch) / "server.log"
        with log_path.open("w+") as log:
            server = Server(args.server.resolve(), database, "twelve-dollar-challenge", log)
            try:
                server.start()
                valid = token(server.secret)
                for path in ("/missing.json", "/posts/1/extra", "/feed/extra"):
                    status, body, _ = server.request("GET", path)
                    check(status == 404 and body == {"error": "not found"}, f"JSON fallback: {path}")

                payloads = [
                    ({"sub": 1, "username": "x", "exp": int(time.time()) + 60}, "invalid token payload"),
                    ({"sub": "0", "username": "x", "exp": int(time.time()) + 60}, "invalid token payload"),
                    ({"sub": "1", "username": 123, "exp": int(time.time()) + 60}, "invalid token payload"),
                    ({"sub": "1", "username": "x"}, "invalid or expired token"),
                    ({"sub": "1", "username": "x", "exp": int(time.time())}, "invalid or expired token"),
                    ({"sub": "1", "username": "x", "exp": int(time.time()) + 120,
                      "nbf": int(time.time()) + 60}, "invalid or expired token"),
                ]
                for index, (payload, error) in enumerate(payloads):
                    status, body, _ = server.request("POST", "/posts", token(server.secret, payload), {"body": "must not insert"})
                    check(status == 401 and body == {"error": error}, f"JWT claims/lifetime case {index + 1}")
                for invalid in ("", "a.b.c", token(server.secret, algorithm="HS384"), token("different-signing-key")):
                    status, body, _ = server.request("POST", "/posts", invalid, {"body": "must not insert"})
                    # Kestrel removes trailing HTTP header whitespace: an empty Bearer value
                    # reaches the application as 'Bearer', which has no 'Bearer ' prefix.
                    error = "missing bearer token" if invalid == "" else "invalid or expired token"
                    check(status == 401 and body == {"error": error}, "malformed/incorrect token rejected")

                for raw in (b"null", b"[]", b'{"body":null}'):
                    status, body, _ = server.request("POST", "/posts", valid, raw=raw)
                    check(status == 400 and body == {"error": "body is required"}, f"JSON shape validation: {raw!r}")
                status, body, _ = server.request("POST", "/posts", valid, raw=b"")
                check(status == 400 and body == {"error": "malformed JSON body"}, "empty JSON input rejected")

                text = "🙂" * 500
                status, body, _ = server.request("POST", "/posts", valid, {"body": "  " + text + "  "})
                check(status == 201 and body["post"]["body"] == text, "500 Unicode code points round-trip")
                post_id = body["post"]["id"]
                status, body, _ = server.request("POST", "/posts", valid, {"body": text + "🙂"})
                check(status == 400 and body == {"error": "body must be at most 500 characters"}, "501 Unicode code points rejected")

                # Simultaneous identical writes must yield exactly one 201, then idempotent 200s.
                with concurrent.futures.ThreadPoolExecutor(max_workers=24) as pool:
                    likes = list(pool.map(lambda _: server.request("POST", f"/posts/{post_id}/like", valid), range(48)))
                check(sum(status == 201 for status, _, _ in likes) == 1 and
                      sum(status == 200 for status, _, _ in likes) == 47,
                      "48 concurrent duplicate likes create one row")
                check(all(body == {"liked": True, "already_liked": status == 200, "post_id": post_id}
                          for status, body, _ in likes), "concurrent like responses match each commit")

                # Different users must all be counted immediately, with concurrent readers.
                def mixed(index):
                    if index % 2 == 0:
                        return server.request("GET", "/feed")
                    user_id = index + 1
                    identity = {"sub": str(user_id), "username": f"user-{user_id}", "exp": int(time.time()) + 3600}
                    return server.request("POST", f"/posts/{post_id}/like", token(server.secret, identity))

                with concurrent.futures.ThreadPoolExecutor(max_workers=24) as pool:
                    mixed_results = list(pool.map(mixed, range(80)))
                check(all(status in (200, 201) for status, _, _ in mixed_results), "concurrent readers and distinct writers succeed")
                check(server.request("GET", f"/posts/{post_id}")[1]["post"]["like_count"] == 41,
                      "committed concurrent likes visible to next read")

                # Reused statements must read fresh rows, not preserve a prior result.
                with closing(sqlite3.connect(database)) as connection:
                    connection.execute("UPDATE posts SET body = ? WHERE id = ?", ("external update", post_id))
                    connection.execute("INSERT INTO likes (user_id, post_id) VALUES (3, ?)", (post_id,))
                    connection.commit()
                observed = server.request("GET", f"/posts/{post_id}")[1]["post"]
                check(observed["body"] == "external update" and observed["like_count"] == 42,
                      "reused post query sees external committed changes")
                observed_feed = server.request("GET", "/feed")[1]["posts"]
                check(any(post["id"] == post_id and post["body"] == "external update" and post["like_count"] == 42
                          for post in observed_feed), "reused feed query sees external committed changes")
                with closing(sqlite3.connect(database)) as connection:
                    connection.execute("DELETE FROM likes WHERE user_id = 3 AND post_id = ?", (post_id,))
                    connection.commit()

                def create(index):
                    return server.request("POST", "/posts", valid, {"body": f"concurrent post {index}"})

                with concurrent.futures.ThreadPoolExecutor(max_workers=24) as pool:
                    posts = list(pool.map(create, range(48)))
                check(all(status == 201 for status, _, _ in posts) and
                      len({body["post"]["id"] for _, body, _ in posts}) == 48,
                      "48 concurrent creates have unique committed IDs")
                persisted_id = posts[-1][1]["post"]["id"]
                with closing(sqlite3.connect(database)) as connection:
                    check(connection.execute("SELECT count(*) FROM posts WHERE id = ?", (persisted_id,)).fetchone()[0] == 1,
                          "201 is visible from a separate database connection")
                    check(connection.execute("PRAGMA foreign_key_check").fetchall() == [], "foreign key integrity holds")
                    before = connection.execute("SELECT count(*) FROM posts").fetchone()[0]
                unknown_user = token(server.secret, {"sub": "999999999", "username": "absent", "exp": int(time.time()) + 3600})
                status, body, _ = server.request("POST", "/posts", unknown_user, {"body": "must roll back"})
                check(status == 500 and body == {"error": "internal server error"}, "foreign key violation gives safe error")
                with closing(sqlite3.connect(database)) as connection:
                    check(connection.execute("SELECT count(*) FROM posts").fetchone()[0] == before, "failed write leaves no partial row")
                check(server.request("POST", "/posts", valid, {"body": "write after constraint failure"})[0] == 201,
                      "writer recovers after a constraint failure")

                # An external writer must not cause an unbounded application queue wait.
                with closing(sqlite3.connect(database)) as blocker:
                    blocker.execute("BEGIN IMMEDIATE")
                    contention_start = time.monotonic()
                    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                        waiting = [pool.submit(create, index) for index in range(8)]
                        check(server.request("GET", "/feed")[0] == 200, "WAL reads succeed while external writer holds a lock")
                        blocked = [future.result() for future in waiting]
                    check(all(status == 500 and body == {"error": "internal server error"} for status, body, _ in blocked),
                          "blocked writes fail with safe contract errors")
                    check(time.monotonic() - contention_start < 6, "write permit waits have a bounded deadline")
                    blocker.rollback()
                check(server.request("POST", "/posts", valid, {"body": "write after contention"})[0] == 201,
                      "writer permit recovers after contention failures")

                server.stop(kill=True)
                server.start()
                check(server.request("GET", f"/posts/{persisted_id}")[0] == 200, "acknowledged post survives process kill/restart")
                check(server.request("GET", f"/posts/{post_id}")[1]["post"]["like_count"] == 41,
                      "acknowledged likes survive process kill/restart")
                with closing(sqlite3.connect(database)) as connection:
                    check(connection.execute("PRAGMA integrity_check").fetchone()[0] == "ok", "database integrity after recovery")
                    backup = Path(scratch) / "backup.db"
                    with closing(sqlite3.connect(backup)) as destination:
                        connection.backup(destination)
                server.stop()
                server.database = backup
                server.start()
                check(server.request("GET", f"/posts/{persisted_id}")[0] == 200, "online backup restores acknowledged data")

                # A real, longer secret must go through the unmodified IdentityModel provider.
                server.stop()
                server.secret = "integration-only-strong-signing-key-at-least-32-bytes"
                server.start()
                status, _, _ = server.request("POST", "/posts", token(server.secret), {"body": "strong-key validation"})
                check(status == 201, "standard IdentityModel validation accepts a stronger secret")
                check(server.request("POST", "/posts", valid, {"body": "wrong key"})[0] == 401,
                      "old challenge signature rejected with stronger secret")
            except BaseException:
                server.stop(kill=True)
                log.flush()
                print(log_path.read_text(), flush=True)
                raise
            finally:
                server.stop()
    print(f"passed={passed} failed=0", flush=True)


if __name__ == "__main__":
    main()
