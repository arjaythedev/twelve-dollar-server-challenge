#!/usr/bin/env python3
"""Black-box worker supervision, inherited limits, and shutdown checks."""
import http.client
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
EXPECTED_WORKERS = 6


def eventually(predicate, label):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.05)
    raise AssertionError(label)


def running(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0] != "Z"
    except FileNotFoundError:
        return False


with tempfile.TemporaryDirectory(prefix="ada-workers-") as temporary:
    db = Path(temporary) / "feed.db"
    shutil.copyfile(ROOT / "seed/feed.db", db)
    env = os.environ | dict(SQLITE_PATH=str(db), JWT_SECRET="worker-test",
                            HOST="127.0.0.1", PORT="3002")
    for trial, kill_child in enumerate((True, False) + (None,) * 12):
        if kill_child is None:
            fresh = Path(temporary) / f"fresh-{trial}.db"
            connection = sqlite3.connect(fresh)
            connection.executescript((ROOT / "schema.sql").read_text())
            assert connection.execute("PRAGMA journal_mode").fetchone()[0] == "delete"
            connection.close()
            env["SQLITE_PATH"] = str(fresh)
        with (Path(temporary) / "server.log").open("w+") as log:
            proc = subprocess.Popen(["bash", str(ADA / "start.sh")], env=env,
                                    stdout=log, stderr=log)
            children = []
            try:
                def ready():
                    global children
                    assert proc.poll() is None, "supervisor exited during startup"
                    children = [int(p) for p in Path(
                        f"/proc/{proc.pid}/task/{proc.pid}/children").read_text().split()]
                    if len(children) != EXPECTED_WORKERS:
                        return False
                    try:
                        # Every worker must have reached its listener after
                        # opening SQLite, not just the first healthy worker.
                        if not all(any(os.readlink(fd).startswith("socket:")
                                       for fd in Path(f"/proc/{pid}/fd").iterdir())
                                   for pid in children):
                            return False
                        conn = http.client.HTTPConnection("127.0.0.1", 3002, timeout=1)
                        try:
                            conn.request("GET", "/health")
                            response = conn.getresponse()
                            return response.status == 200 and b'"db":"ok"' in response.read()
                        finally:
                            conn.close()
                    except OSError:
                        return False
                eventually(ready, "all workers start")
                for pid in children:
                    limit = next(line.split() for line in Path(f"/proc/{pid}/limits").read_text().splitlines()
                                 if line.startswith("Max open files"))
                    assert int(limit[3]) <= 65535, limit
                if kill_child:
                    os.kill(children[0], signal.SIGKILL)
                    assert proc.wait(timeout=10) != 0, "child death must fail supervisor"
                elif kill_child is False:
                    proc.kill()
                    proc.wait(timeout=10)
                else:
                    proc.terminate()
                    proc.wait(timeout=10)
                eventually(lambda: not any(running(pid) for pid in children),
                           "worker processes left behind")
                print("PASS:", "child failure shuts down siblings" if kill_child else
                      "supervisor death shuts down children" if kill_child is False else
                      f"all {EXPECTED_WORKERS} workers start on fresh rollback-mode database {trial - 1}", flush=True)
            except BaseException:
                log.seek(0)
                print(log.read(), flush=True)
                raise
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=10)
