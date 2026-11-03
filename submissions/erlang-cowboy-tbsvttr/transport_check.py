#!/usr/bin/env python3
"""Socket-level HTTP regression checks for the Erlang transport, against a fresh seed."""
import base64
import hashlib
import hmac
import json
import os
import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18084
secret = os.getenv("JWT_SECRET", "twelve-dollar-challenge").encode()
b64 = lambda b: base64.urlsafe_b64encode(b).rstrip(b"=")
claim = {"sub": "1", "username": "transport ✓", "exp": int(time.time()) + 3600}
signed = b64(b'{"alg":"HS256"}') + b"." + b64(json.dumps(claim).encode())
token = (signed + b"." + b64(hmac.new(secret, signed, hashlib.sha256).digest())).decode()


class Connection:
    def __init__(self, receive_buffer=None):
        self.socket = socket.socket()
        if receive_buffer:
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        self.socket.settimeout(15)
        self.socket.connect(("127.0.0.1", PORT))
        self.file = self.socket.makefile("rb")

    def send(self, raw):
        self.socket.sendall(raw)

    def read(self):
        line = self.file.readline()
        assert line.startswith(b"HTTP/1.1 "), ("missing HTTP status", line)
        status = int(line.split()[1])
        headers = {}
        while True:
            line = self.file.readline()
            assert line, "unexpected EOF in headers"
            if line == b"\r\n":
                break
            k, v = line.decode().split(":", 1)
            headers[k.lower()] = v.strip()
        body = self.file.read(int(headers.get("content-length", "0")))
        return status, json.loads(body)

    def close(self):
        self.file.close()
        self.socket.close()


def request_head(method, path, extra=""):
    return (f"{method} {path} HTTP/1.1\r\nHost: localhost\r\n{extra}\r\n").encode()


def quick(path="/health"):
    con = Connection()
    con.send(request_head("GET", path))
    response = con.read()
    con.close()
    return response


def headers(length=None, chunked=False):
    extra = f"Authorization: Bearer {token}\r\nContent-Type: application/json\r\n"
    if chunked:
        return extra + "Transfer-Encoding: chunked\r\n"
    return extra + f"Content-Length: {length}\r\n"


idle = Connection()
idle.send(request_head("GET", "/health"))
assert idle.read()[0] == 200
idle_started = time.monotonic()
print("Opened keep-alive socket for 66-second reuse check", flush=True)

# Parser receive storage is reused by unrelated requests between POST chunks.
payload = json.dumps({"body": "fragmented ✓ body"}, ensure_ascii=False).encode()
for chunked in [False, True]:
    con = Connection()
    con.send(request_head("POST", "/posts", headers(len(payload), chunked)))
    chunks = [payload[:5], payload[5:13], payload[13:]]
    for index, chunk in enumerate(chunks):
        con.send((f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n") if chunked else chunk)
        for _ in range(6):
            assert quick("/posts/500000")[0] == 200
    if chunked:
        con.send(b"0\r\n\r\n")
    status, body = con.read()
    assert status == 201 and body["post"]["body"] == "fragmented ✓ body", (status, body)
    assert body["post"]["author"] == "transport ✓", body
    # Same socket receives an empty-body POST after its previous data handler finishes.
    con.send(request_head("POST", f'/posts/{body["post"]["id"]}/like', headers(0)))
    assert con.read()[0] == 201
    con.close()
print("PASS fragmented and chunked POST, owned headers, empty-body reuse", flush=True)

# Aborted uploads must discard their strings and callback state without writing.
for _ in range(100):
    con = Connection()
    con.send(request_head("POST", "/posts", headers(1000)) + b'{"body":"unfinished')
    con.close()
assert quick()[0] == 200
print("PASS 100 aborted partial uploads", flush=True)

# Read only after enough responses have queued to force socket backpressure.
baseline_post = quick("/posts/500000")
baseline_feed = quick("/feed")
paths = ["/feed", "/posts/500000", "/health"] * 600
con = Connection(65536)
con.send(b"".join(request_head("GET", path) for path in paths))
time.sleep(0.3)
for path in paths:
    actual = con.read()
    if path == "/feed":
        assert actual == baseline_feed, "feed response storage corrupted"
    elif path == "/posts/500000":
        assert actual == baseline_post, "post response storage corrupted"
    else:
        assert actual[0] == 200 and actual[1]["db"] == "ok", actual
con.close()
print("PASS 1,800 pipelined responses under backpressure", flush=True)

# Keep the original socket object: a client library must not silently reconnect.
while time.monotonic() - idle_started < 66:
    time.sleep(min(15, 66 - (time.monotonic() - idle_started)))
idle.send(request_head("GET", "/health"))
assert idle.read()[0] == 200
idle.close()
print("PASS same-socket HTTP keep-alive after 66 idle seconds", flush=True)
