#!/usr/bin/env python3
"""Black-box process lifecycle checks: one single-threaded server process
plus its checkpoint helper (which exits with the server), fresh
rollback-journal databases switch to WAL, SIGTERM/SIGKILL keep committed
writes, and the descriptor limit stays within the inherited hard limit."""
import http.client
import json
import os
from pathlib import Path
import shutil
import signal
import sqlite3
import subprocess
import tempfile
import time

ADA = Path(__file__).resolve().parents[1]
ROOT = ADA.parents[1]
PORT = 3002


def request(method, path, body=None, token=None):
    conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=5)
    try:
        headers = {"Authorization": "Bearer " + token} if token else {}
        conn.request(method, path, body=body, headers=headers)
        response = conn.getresponse()
        return response.status, response.read()
    finally:
        conn.close()


def wait_ready(proc):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        assert proc.poll() is None, "server exited during startup"
        try:
            status, content = request("GET", "/health")
            if status == 200 and b'"db":"ok"' in content:
                return
        except OSError:
            pass
        time.sleep(.05)
    raise AssertionError("server not ready")


def sign(secret, payload):
    import base64, hashlib, hmac
    b64 = lambda raw: base64.urlsafe_b64encode(raw).rstrip(b"=")
    data = b64(b'{"alg":"HS256","typ":"JWT"}') + b"." + b64(json.dumps(payload).encode())
    return (data + b"." + b64(hmac.digest(secret.encode(), data, hashlib.sha256))).decode()


with tempfile.TemporaryDirectory(prefix="ada-lifecycle-") as temporary:
    seed = Path(temporary) / "feed.db"
    shutil.copyfile(ROOT / "seed/feed.db", seed)
    token = sign("lifecycle-test", {"sub": "1", "username": "life", "exp": time.time() + 3600})
    cases = [("seed", seed, signal.SIGTERM), ("seed", seed, signal.SIGKILL)]
    for trial in range(6):
        fresh = Path(temporary) / f"fresh-{trial}.db"
        connection = sqlite3.connect(fresh)
        connection.executescript((ROOT / "schema.sql").read_text())
        connection.execute("INSERT INTO users(id,username) VALUES(1,'life')")
        connection.commit()
        assert connection.execute("PRAGMA journal_mode").fetchone()[0] == "delete"
        connection.close()
        cases.append((f"fresh rollback-mode database {trial}", fresh, signal.SIGTERM))
    for label, db, stop in cases:
        env = os.environ | dict(SQLITE_PATH=str(db), JWT_SECRET="lifecycle-test",
                                HOST="127.0.0.1", PORT=str(PORT))
        with (Path(temporary) / "server.log").open("w+") as log:
            proc = subprocess.Popen(["bash", str(ADA / "start.sh")], env=env, stdout=log, stderr=log)
            try:
                wait_ready(proc)
                status = Path(f"/proc/{proc.pid}/status").read_text()
                assert "Threads:\t1\n" in status, status
                children = Path(f"/proc/{proc.pid}/task/{proc.pid}/children").read_text().split()
                assert len(children) == 1, f"one child (the checkpoint helper), got {children}"
                helper = int(children[0])
                limit = next(line.split() for line in Path(f"/proc/{proc.pid}/limits").read_text().splitlines()
                             if line.startswith("Max open files"))
                assert int(limit[3]) <= int(limit[4]), limit
                status, content = request("POST", "/posts", b'{"body":"lifecycle"}', token)
                assert status == 201, content
                post_id = json.loads(content)["post"]["id"]
                status, _ = request("POST", f"/posts/{post_id}/like", None, token)
                assert status == 201
                proc.send_signal(stop)
                proc.wait(timeout=10)
                # The helper notices its parent is gone and exits on its own.
                deadline = time.monotonic() + 2
                while Path(f"/proc/{helper}").exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                assert not Path(f"/proc/{helper}").exists(), "checkpoint helper outlived the server"
                with sqlite3.connect(db) as reader:
                    assert reader.execute("PRAGMA journal_mode").fetchone()[0] == "wal"
                    assert reader.execute("SELECT count(*) FROM likes WHERE post_id=?", (post_id,)).fetchone()[0] == 1
                log.seek(0)
                backend = next((line.strip() for line in log if line.startswith(("io_uring", "epoll"))), "?")
                print(f"PASS: {label}: one thread, {backend}; writes durable after {stop.name}", flush=True)
            except BaseException:
                log.seek(0)
                print(log.read(), flush=True)
                raise
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=10)
