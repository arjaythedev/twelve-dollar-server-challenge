"""Exercise HTTP framing, pipelined writes, interim responses and half-close."""
import base64
import hashlib
import hmac
import json
import socket
import sys
import time

port = int(sys.argv[1])
b64 = lambda value: base64.urlsafe_b64encode(json.dumps(value).encode()).rstrip(b"=")
signed = b64({"alg": "HS256"}) + b"." + b64({"sub": "1", "username": "pipeline", "exp": time.time() + 3600})
auth = signed + b"." + base64.urlsafe_b64encode(hmac.new(b"twelve-dollar-challenge", signed, hashlib.sha256).digest()).rstrip(b"=")
payload = b'{"body":"pipelined write"}'


def head(method="GET", path="/health", extra=b"", version="1.1"):
    return f"{method} {path} HTTP/{version}\r\nHost: localhost\r\n".encode() + extra + b"\r\n"


def post_headers(extra=b""):
    return b"Authorization: Bearer " + auth + b"\r\n" + extra


def read(file):
    status = int(file.readline().split()[1])
    headers = {}
    while (line := file.readline()) != b"\r\n":
        assert line, "EOF in response headers"
        name, value = line.split(b":", 1)
        headers[name.lower()] = value.strip()
    body = file.read(int(headers.get(b"content-length", b"0")))
    return status, json.loads(body) if body else None


invalid = [
    b"Content-Length: 0\r\nContent-Length: 0\r\n",
    b"Content-Length: 0\r\nTransfer-Encoding: chunked\r\n",
    b"Content-Length: -1\r\n", b"Content-Length: +1\r\n",
    b"Transfer-Encoding: gzip, chunked\r\n",
    b"Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n",
    b"Host: second\r\n", b"X-Header: x\r\n folded\r\n",
    b"Authorization: a\r\nAuthorization: b\r\n",
]
for extra in invalid:
    with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
        with sock.makefile("rb") as file:
            sock.sendall(head(extra=extra) + head())
            assert read(file)[0] == 400, extra
            assert file.read(1) == b"", "framing error must close the connection"
print("PASS ambiguous framing, duplicate headers and obsolete folding rejected", flush=True)

with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
    with sock.makefile("rb") as file:
        sock.sendall(head("POST", "/posts", post_headers(f"Content-Length: {len(payload)}\r\nExpect: 100-continue\r\n".encode())))
        assert read(file) == (100, None)
        sock.sendall(payload + head())
        assert read(file)[0] == 201
        assert read(file)[0] == 200
print("PASS Expect: 100-continue followed by a pipelined request", flush=True)

with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
    with sock.makefile("rb") as file:
        chunk = f"{len(payload):x};extension=yes\r\n".encode() + payload + b"\r\n0\r\nX-Trailer: done\r\n\r\n"
        sock.sendall(head("POST", "/posts", post_headers(b"Transfer-Encoding: chunked\r\n")) + chunk + head())
        assert read(file)[0] == 201
        assert read(file)[0] == 200
print("PASS chunk extensions, trailers and preservation of pipelined bytes", flush=True)

with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
    with sock.makefile("rb") as file:
        write = head("POST", "/posts", post_headers(f"Content-Length: {len(payload)}\r\n".encode())) + payload
        sock.sendall((write + head("GET", "/feed")) * 300)
        ids = []
        for _ in range(300):
            status, created = read(file)
            assert status == 201, (status, created)
            ids.append(created["post"]["id"])
            status, feed = read(file)
            assert status == 200 and feed["posts"][0]["id"] == ids[-1]
        assert len(set(ids)) == 300 and ids == sorted(ids)
print("PASS 600 ordered pipelined write/read responses across bounded commit batches", flush=True)

with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
    with sock.makefile("rb") as file:
        sock.sendall(write + head("GET", "/feed"))
        sock.shutdown(socket.SHUT_WR)
        assert read(file)[0] == 201
        assert read(file)[0] == 200
        assert file.read(1) == b""
print("PASS peer half-close preserves responses waiting for commit", flush=True)
