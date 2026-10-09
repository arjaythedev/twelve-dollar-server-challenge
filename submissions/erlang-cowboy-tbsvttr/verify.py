#!/usr/bin/env python3
"""Run correctness, transport and acknowledged-write recovery checks on a fresh DB."""
import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import hmac
import http.client
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PORT = int(os.getenv("TEST_PORT", "19800"))
SECRET = "twelve-dollar-challenge"


def token(user="1"):
    encode = lambda value: base64.urlsafe_b64encode(json.dumps(value).encode()).rstrip(b"=")
    signed = encode({"alg": "HS256"}) + b"." + encode(
        {"sub": user, "username": "batch recovery", "exp": time.time() + 3600})
    return (signed + b"." + base64.urlsafe_b64encode(
        hmac.new(SECRET.encode(), signed, hashlib.sha256).digest()).rstrip(b"=")).decode()


def request(method, path, body=None, user="1"):
    connection = http.client.HTTPConnection("127.0.0.1", PORT, timeout=15)
    try:
        connection.request(method, path, None if body is None else json.dumps(body),
                           {"Authorization": "Bearer " + token(user), "Content-Type": "application/json"})
        response = connection.getresponse()
        return response.status, json.loads(response.read())
    finally:
        connection.close()


with tempfile.TemporaryDirectory(prefix="twelve-batch-verify-") as directory:
    database = Path(directory) / "feed.db"
    shutil.copyfile(ROOT / "seed/feed.db", database)
    env = dict(os.environ, SQLITE_PATH=str(database), JWT_SECRET=SECRET,
               HOST="127.0.0.1", PORT=str(PORT))
    process = None
    with (Path(directory) / "server.log").open("w+") as log:
        def start():
            server = subprocess.Popen(["bash", str(HERE / "start.sh")], env=env, stdout=log, stderr=log)
            for _ in range(100):
                if server.poll() is not None:
                    log.seek(0)
                    raise RuntimeError(log.read())
                try:
                    if request("GET", "/health")[0] == 200:
                        return server
                except OSError:
                    pass
                time.sleep(0.05)
            server.kill()
            server.wait()
            raise RuntimeError("server startup timed out")

        try:
            process = start()
            subprocess.run(["bash", str(ROOT / "test/test.sh"), f"http://127.0.0.1:{PORT}"], env=env, check=True)
            subprocess.run(["python3", str(HERE / "check.py"), f"http://127.0.0.1:{PORT}"], env=env, check=True)
            subprocess.run(["python3", str(HERE / "transport_check.py"), str(PORT)], env=env, check=True)

            # Failed FK writes must not undo other successful writes in the batch.
            def create(index):
                user = "999999999" if index % 7 == 0 else "1"
                status, data = request("POST", "/posts", {"body": f"durable batch {index}"}, user)
                assert status == (500 if user != "1" else 201), (index, status, data)
                return None if status == 500 else data["post"]

            with ThreadPoolExecutor(max_workers=32) as pool:
                posts = [post for post in pool.map(create, range(224)) if post is not None]
                likes = list(pool.map(lambda post: request("POST", f'/posts/{post["id"]}/like'), posts))
            assert len(posts) == 192 and all(status == 201 for status, _ in likes)
            process.kill()
            process.wait()
            process = start()
            for post in posts:
                status, stored = request("GET", f'/posts/{post["id"]}')
                assert status == 200 and stored["post"]["body"] == post["body"]
                assert stored["post"]["like_count"] == 1
            print("PASS 192 acknowledged creates and likes survive SIGKILL; 32 failed writes remain isolated", flush=True)
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
