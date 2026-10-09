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
import socket
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


def pipeline():
    body = b'{"body":"uncommitted pipeline"}'
    return (f"POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {token}\r\n"
            f"Content-Length: {len(body)}\r\n\r\n").encode() + body + (
            b"GET /posts/500002 HTTP/1.1\r\nHost: localhost\r\n\r\n")


def read(file):
    status = int(file.readline().split()[1])
    headers = {}
    while (line := file.readline()) != b"\r\n":
        assert line, "EOF in headers"
        name, value = line.split(b":", 1)
        headers[name.lower()] = value.strip()
    return status, json.loads(file.read(int(headers[b"content-length"])))


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

        (directory / "fail").touch()
        with socket.create_connection(("127.0.0.1", PORT), timeout=10) as sock:
            with sock.makefile("rb") as file:
                sock.sendall(pipeline())
                assert read(file) == (500, {"error": "internal server error"})
                assert read(file) == (500, {"error": "internal server error"})
        assert request("GET", "/posts/500002")[0] == 404
        print("PASS failed batch releases neither write success nor uncommitted read data", flush=True)

        (directory / "entered").unlink(missing_ok=True)
        (directory / "hold").touch()
        with socket.create_connection(("127.0.0.1", PORT), timeout=10) as sock:
            sock.sendall(pipeline())
            try:
                for _ in range(100):
                    if (directory / "entered").exists():
                        break
                    time.sleep(0.01)
                assert (directory / "entered").exists()
                sock.settimeout(0.2)
                try:
                    assert sock.recv(1) == b"", "response byte escaped before commit"
                    raise AssertionError("connection closed while commit was pending")
                except socket.timeout:
                    pass
            finally:
                (directory / "hold").unlink(missing_ok=True)
            sock.settimeout(10)
            with sock.makefile("rb") as file:
                assert read(file)[0] == 201
                status, result = read(file)
                assert status == 200 and result["post"]["body"] == "uncommitted pipeline"
        print("PASS pipelined writes and dependent reads wait for the same commit", flush=True)

        (directory / "fail").touch()
        assert request("POST", "/posts/500002/like")[0] == 500
        assert request("GET", "/posts/500002")[1]["post"]["like_count"] == 0
        assert request("POST", "/posts/500002/like")[0] == 201
        print("PASS failed like commit rolls back and a subsequent like succeeds", flush=True)
    finally:
        process.kill()
        process.wait()
