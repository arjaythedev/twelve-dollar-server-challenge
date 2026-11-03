"""Raw-socket HTTP/1.1 edge cases: python3 http_edge.py PORT TOKEN"""
import json
import socket
import sys
import time

PORT = int(sys.argv[1])
TOKEN = sys.argv[2]
ok = fail = 0


def check(name, cond, detail=""):
    global ok, fail
    if cond:
        ok += 1
        print("  ok  ", name)
    else:
        fail += 1
        print("  FAIL", name, detail)


def conn():
    s = socket.create_connection(("127.0.0.1", PORT))
    s.settimeout(5)
    return s


def read_responses(s, n):
    """Read n complete responses (Content-Length framed). Returns list of (status, headers, body)."""
    buf = b""
    out = []
    while len(out) < n:
        while True:
            he = buf.find(b"\r\n\r\n")
            if he >= 0:
                head = buf[:he].decode()
                lines = head.split("\r\n")
                status = int(lines[0].split()[1])
                headers = {k.lower(): v.strip() for k, v in (l.split(":", 1) for l in lines[1:])}
                if status == 100:
                    out.append((100, headers, b""))
                    buf = buf[he + 4:]
                    break
                cl = int(headers["content-length"])
                if len(buf) >= he + 4 + cl:
                    out.append((status, headers, buf[he + 4:he + 4 + cl]))
                    buf = buf[he + 4 + cl:]
                    break
            chunk = s.recv(65536)
            if not chunk:
                return out
            buf += chunk
    return out


# 1. keep-alive: several sequential requests on one connection
s = conn()
for i in range(3):
    s.sendall(b"GET /posts/1 HTTP/1.1\r\nHost: x\r\n\r\n")
    r = read_responses(s, 1)
    check(f"keep-alive request {i}", r and r[0][0] == 200 and "date" in r[0][1])
s.close()

# 2. pipelining: 3 requests in one packet, responses in order
s = conn()
s.sendall(b"GET /posts/1 HTTP/1.1\r\nHost: x\r\n\r\nGET /posts/abc HTTP/1.1\r\nHost: x\r\n\r\nGET /nope HTTP/1.1\r\nHost: x\r\n\r\n")
r = read_responses(s, 3)
check("pipelined responses in order", [x[0] for x in r] == [200, 400, 404], [x[0] for x in r])
s.close()

# 3. request split byte by byte
s = conn()
req = b"POST /posts/1/like HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + TOKEN.encode() + b"\r\nContent-Length: 0\r\n\r\n"
for i in range(len(req)):
    s.send(req[i:i + 1])
    time.sleep(0.0005)
r = read_responses(s, 1)
check("byte-by-byte request", r and r[0][0] in (200, 201), r[:1])
s.close()

# 4. body arrives later than headers
s = conn()
body = json.dumps({"body": "split body"}).encode()
s.sendall(b"POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + TOKEN.encode() +
          b"\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n" % len(body))
time.sleep(0.05)
s.sendall(body[:5])
time.sleep(0.05)
s.sendall(body[5:])
r = read_responses(s, 1)
check("body in separate packets", r and r[0][0] == 201 and json.loads(r[0][2])["post"]["body"] == "split body", r[:1])
s.close()

# 5. Expect: 100-continue
s = conn()
body = json.dumps({"body": "expect continue"}).encode()
s.sendall(b"POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + TOKEN.encode() +
          b"\r\nExpect: 100-continue\r\nContent-Length: %d\r\n\r\n" % len(body))
r = read_responses(s, 1)
check("100 Continue sent", r and r[0][0] == 100, r[:1])
s.sendall(body)
r = read_responses(s, 1)
check("expect-continue body accepted", r and r[0][0] == 201, r[:1])
s.close()

# 6. chunked request body
s = conn()
s.sendall(b"POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + TOKEN.encode() +
          b"\r\nTransfer-Encoding: chunked\r\n\r\n5\r\n{\"bod\r\n10\r\ny\":\"chunked ok\"}\r\n0\r\n\r\n")
r = read_responses(s, 1)
check("chunked body", r and r[0][0] == 201 and json.loads(r[0][2])["post"]["body"] == "chunked ok", r[:1])
s.close()

# 7. HTTP/1.0 closes by default; Connection: close honoured
for name, req in [("HTTP/1.0 closes", b"GET /posts/1 HTTP/1.0\r\n\r\n"),
                  ("Connection: close", b"GET /posts/1 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")]:
    s = conn()
    s.sendall(req)
    r = read_responses(s, 1)
    rest = s.recv(100)
    check(name, r and r[0][0] == 200 and r[0][1].get("connection") == "close" and rest == b"", (r[:1], rest))
    s.close()
s = conn()
s.sendall(b"GET /posts/1 HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
r = read_responses(s, 1)
s.sendall(b"GET /posts/2 HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
r += read_responses(s, 1)
check("HTTP/1.0 keep-alive", [x[0] for x in r] == [200, 200])
s.close()

# 8. query string ignored, header case-insensitive, oversized header rejected
s = conn()
s.sendall(b"GET /feed?x=1 HTTP/1.1\r\nhOsT: x\r\n\r\n")
r = read_responses(s, 1)
check("query string ignored", r and r[0][0] == 200 and len(json.loads(r[0][2])["posts"]) == 20)
s.sendall(b"POST /posts/1/like HTTP/1.1\r\nHost: x\r\nauthorization: Bearer " + TOKEN.encode() + b"\r\ncontent-length: 0\r\n\r\n")
r = read_responses(s, 1)
check("lower-case header names", r and r[0][0] in (200, 201), r[:1])
s.close()
s = conn()
s.sendall(b"GET /feed HTTP/1.1\r\nX-Big: " + b"a" * 20000)
r = read_responses(s, 1)
check("oversized header -> 431", r and r[0][0] == 431, r[:1])
s.close()

# 9. many pipelined feeds without reading (forces partial writes), then read all
s = conn()
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
n = 400
s.sendall(b"GET /feed HTTP/1.1\r\nHost: x\r\n\r\n" * n)
time.sleep(0.5)
r = read_responses(s, n)
check(f"{n} pipelined feeds with a slow reader", len(r) == n and all(x[0] == 200 for x in r), len(r))
s.sendall(b"GET /posts/1 HTTP/1.1\r\nHost: x\r\n\r\n")
r = read_responses(s, 1)
check("connection usable after partial writes", r and r[0][0] == 200)
s.close()

# 10. client disconnects mid-request; server stays healthy
for i in range(50):
    s = conn()
    s.sendall(b"POST /posts HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\n{\"bo")
    s.close()
s = conn()
s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
r = read_responses(s, 1)
check("healthy after aborted requests", r and r[0][0] == 200)
s.close()

# 11. garbage request line
s = conn()
s.sendall(b"HELLO\r\n\r\n")
r = read_responses(s, 1)
check("garbage request -> 400", r and r[0][0] == 400, r[:1])
s.close()

# 12. Content-Length together with Transfer-Encoding, and an unknown transfer coding, are refused
for name, req in [("Content-Length + Transfer-Encoding -> 400",
                   b"POST /posts HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"),
                  ("Transfer-Encoding: gzip -> 400",
                   b"POST /posts HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n")]:
    s = conn()
    s.sendall(req)
    r = read_responses(s, 1)
    check(name, r and r[0][0] == 400, r[:1])
    s.close()

# 13. Connection header is a token list
s = conn()
s.sendall(b"GET /posts/1 HTTP/1.1\r\nHost: x\r\nConnection: TE, close\r\n\r\n")
r = read_responses(s, 1)
rest = s.recv(100)
check("Connection: TE, close closes", r and r[0][1].get("connection") == "close" and rest == b"", (r[:1], rest))
s.close()
s = conn()
s.sendall(b"GET /posts/1 HTTP/1.1\r\nHost: x\r\nConnection: closed-captions\r\n\r\nGET /posts/2 HTTP/1.1\r\nHost: x\r\n\r\n")
r = read_responses(s, 2)
check("Connection: closed-captions keeps alive", [x[0] for x in r] == [200, 200], [x[0] for x in r])
s.close()

# 14. a client that pipelines without ever reading is cut off, not buffered without limit
s = conn()
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
sent = 0
s.settimeout(10)
try:
    while sent < 64 << 20:
        s.sendall(b"GET /feed HTTP/1.1\r\nHost: x\r\n\r\n" * 1000)
        sent += 32000
    cut = False
except OSError:
    cut = True
check("non-reading client cut off", cut, sent)
s.close()
s = conn()
s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
r = read_responses(s, 1)
check("healthy after non-reading client", r and r[0][0] == 200)
s.close()

print(f"passed={ok} failed={fail}")
sys.exit(1 if fail else 0)
