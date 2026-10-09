"""Assert commit failure rollback and that success is withheld until commit finishes."""
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

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[1]
PORT = int(os.getenv("TEST_PORT", "19801"))
b64 = lambda value: base64.urlsafe_b64encode(json.dumps(value).encode()).rstrip(b"=")
signed = b64({"alg": "HS256"}) + b"." + b64({"sub": "1", "username": "commit test", "exp": time.time() + 3600})
token = (signed + b"." + base64.urlsafe_b64encode(
    hmac.new(b"twelve-dollar-challenge", signed, hashlib.sha256).digest()).rstrip(b"=")).decode()


def request(method, path, body=None):
    connection = http.client.HTTPConnection("127.0.0.1", PORT, timeout=15)
    try:
        connection.request(method, path, None if body is None else json.dumps(body),
                           {"Authorization": "Bearer " + token, "Content-Type": "application/json"})
        response = connection.getresponse()
        return response.status, json.loads(response.read())
    finally:
        connection.close()


with tempfile.TemporaryDirectory(prefix="twelve-commit-test-") as directory:
    directory = Path(directory)
    shutil.copyfile(ROOT / "seed/feed.db", directory / "feed.db")
    env = dict(os.environ, SQLITE_PATH=str(directory / "feed.db"), COMMIT_TEST_DIR=str(directory),
               JWT_SECRET="twelve-dollar-challenge", HOST="127.0.0.1", PORT=str(PORT))
    process = subprocess.Popen([str(HERE / "bin/commit-test")], env=env)
    try:
        for _ in range(100):
            assert process.poll() is None, "server exited"
            try:
                if request("GET", "/health")[0] == 200:
                    break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError("server startup timed out")
        (directory / "fail").touch()
        assert request("POST", "/posts", {"body": "rolled back"}) == (500, {"error": "internal server error"})
        assert request("GET", "/posts/500001")[0] == 404
        print("PASS failed commit returns 500 and rolls back the inserted post", flush=True)

        (directory / "hold").touch()
        with ThreadPoolExecutor(max_workers=1) as pool:
            response = pool.submit(request, "POST", "/posts", {"body": "committed"})
            try:
                for _ in range(100):
                    if (directory / "entered").exists():
                        break
                    time.sleep(0.01)
                assert (directory / "entered").exists(), "commit hook was not reached"
                time.sleep(0.2)
                assert not response.done(), "response was sent before commit"
            finally:
                (directory / "hold").unlink(missing_ok=True)
            status, body = response.result(timeout=10)
        assert status == 201 and body["post"]["id"] == 500001
        assert request("GET", "/posts/500001")[1]["post"]["body"] == "committed"
        print("PASS success waits for commit; next write succeeds after rollback", flush=True)
    finally:
        process.kill()
        process.wait()
