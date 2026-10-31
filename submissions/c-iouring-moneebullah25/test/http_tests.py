#!/usr/bin/env python3
"""Black-box tests beyond the challenge suite. Standard library only.

Usage: test/http_tests.py --server bin/server --seed ../../seed/feed.db [--long]

Starts its own server instances on fresh copies of the seed (each test group
that writes gets a fresh database), so it can also SIGKILL and restart them.
"""
import argparse
import base64
import hashlib
import hmac
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

SECRET = "twelve-dollar-challenge"
HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN = os.path.join(HERE, "..", "..", "..", "test", "golden")

passed = 0
failed = 0
failures = []


def ok(name):
    global passed
    passed += 1
    print(f"  ok   {name}")


def bad(name, detail=""):
    global failed
    failed += 1
    failures.append(name)
    print(f"  FAIL {name}" + (f"\n       {detail}" if detail else ""))


def check(cond, name, detail=""):
    if cond:
        ok(name)
    else:
        bad(name, detail)


def b64url(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def sign(payload, header=None, secret=SECRET):
    h = b64url(json.dumps(header or {"alg": "HS256", "typ": "JWT"}, separators=(",", ":")).encode())
    p = b64url(json.dumps(payload, separators=(",", ":")).encode())
    s = b64url(hmac.new(secret.encode(), f"{h}.{p}".encode(), hashlib.sha256).digest())
    return f"{h}.{p}.{s}"


def token(sub="1", username="golden_ember_1", exp_delta=3600, **extra):
    now = int(time.time())
    payload = {"sub": sub, "username": username, "iat": now, "exp": now + exp_delta}
    payload.update(extra)
    for k, v in extra.items():
        if v is None:
            del payload[k]
    return sign(payload)


class Server:
    def __init__(self, binary, seed, args=(), port=None):
        self.binary = binary
        self.seed = seed
        self.args = list(args)
        self.port = port or free_port()
        self.dir = tempfile.mkdtemp(prefix="tds-")
        self.db = os.path.join(self.dir, "feed.db")
        shutil.copyfile(seed, self.db)
        self.proc = None
        self.log = open(os.path.join(self.dir, "server.log"), "ab")

    def start(self, extra_args=()):
        env = dict(os.environ, SQLITE_PATH=self.db, JWT_SECRET=SECRET, HOST="127.0.0.1", PORT=str(self.port))
        self.proc = subprocess.Popen([self.binary] + self.args + list(extra_args), env=env, stdout=self.log,
                                     stderr=subprocess.STDOUT)
        deadline = time.time() + 60
        while time.time() < deadline:
            try:
                st, _, body = request(self.port, "GET", "/health")
                if st == 200:
                    return self
            except OSError:
                pass
            if self.proc.poll() is not None:
                raise RuntimeError("server exited: " + open(os.path.join(self.dir, "server.log")).read()[-2000:])
            time.sleep(0.1)
        raise RuntimeError("server did not become healthy")

    def kill9(self):
        self.proc.send_signal(signal.SIGKILL)
        self.proc.wait()

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        log = self.logtail(200000)
        if "runtime error:" in log or "AddressSanitizer" in log or "LeakSanitizer" in log:
            bad("sanitizer report in server log", log[-1500:])
        return self.proc.returncode

    def cleanup(self):
        self.stop()
        self.log.close()
        shutil.rmtree(self.dir, ignore_errors=True)

    def logtail(self, n=3000):
        with open(os.path.join(self.dir, "server.log")) as f:
            return f.read()[-n:]


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def connect(port, timeout=10):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def build(method, path, headers=None, body=None, version="1.1"):
    h = dict(headers or {})
    if body is not None:
        if isinstance(body, str):
            body = body.encode()
        h.setdefault("Content-Length", str(len(body)))
    lines = [f"{method} {path} HTTP/{version}"] + [f"{k}: {v}" for k, v in h.items()]
    return ("\r\n".join(lines) + "\r\n\r\n").encode() + (body or b"")


def read_response(s, buf=b""):
    """Reads one response from the socket; returns (status, headers, body, leftover)."""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(65536)
        if not chunk:
            raise ConnectionError("closed before headers; got %r" % buf[:200])
        buf += chunk
    head, rest = buf.split(b"\r\n\r\n", 1)
    lines = head.decode().split("\r\n")
    status = int(lines[0].split(" ")[1])
    headers = {}
    for ln in lines[1:]:
        k, v = ln.split(":", 1)
        headers[k.strip().lower()] = v.strip()
    n = int(headers.get("content-length", "0"))
    while len(rest) < n:
        chunk = s.recv(65536)
        if not chunk:
            raise ConnectionError("closed mid-body")
        rest += chunk
    return status, headers, rest[:n], rest[n:]


def request(port, method, path, headers=None, body=None, version="1.1", sock=None):
    s = sock or connect(port)
    s.sendall(build(method, path, headers, body, version))
    st, h, b, _ = read_response(s)
    if not sock:
        s.close()
    return st, h, b


def is_closed(s, wait=1.0):
    s.settimeout(wait)
    try:
        return s.recv(1) == b""
    except socket.timeout:
        return False
    except OSError:
        return True


# ----------------------------------------------------------------------------- tests


def test_exact_bytes(srv):
    print("-- exact bytes and headers")
    s = connect(srv.port)
    s.sendall(build("GET", "/feed"))
    st, h, b, rest = read_response(s)
    golden = open(os.path.join(GOLDEN, "feed.json"), "rb").read().rstrip(b"\n")
    check(st == 200 and b == golden, "GET /feed bytes equal the golden file")
    check(h.get("content-type") == "application/json", "content-type exact", h.get("content-type"))
    check(h.get("content-length") == str(len(golden)), "content-length matches body")
    check(rest == b"", "nothing after the body")
    check("connection" not in h, "no Connection header on a keep-alive response")
    raw = s.recv if False else None
    s.close()
    # the status line and header order, byte for byte
    s = connect(srv.port)
    s.sendall(build("GET", "/posts/1"))
    data = b""
    while b"\r\n\r\n" not in data:
        data += s.recv(4096)
    head = data.split(b"\r\n\r\n")[0]
    check(head.startswith(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "),
          "status line and header layout", head[:80])
    s.close()
    st, h, b = request(srv.port, "GET", "/nope")
    check(b == b'{"error":"not found"}' and st == 404, "404 body bytes")
    # key order of a post object
    obj = json.loads(b"{}" if False else request(srv.port, "GET", "/posts/1")[2])
    check(list(obj["post"].keys()) == ["id", "body", "created_at", "author", "like_count"], "post key order")
    feed = json.loads(request(srv.port, "GET", "/feed")[2])
    check(list(feed.keys()) == ["posts"] and all(
        list(p.keys()) == ["id", "body", "created_at", "author", "like_count"] for p in feed["posts"]),
          "feed key order")
    keys = [(p["created_at"], p["id"]) for p in feed["posts"]]
    check(keys == sorted(keys, reverse=True), "feed ordered by created_at desc, id desc")


def test_routing(srv):
    print("-- routing and validation")
    cases = [
        ("GET", "/posts", 404, b'{"error":"not found"}'),
        ("GET", "/posts/", 404, b'{"error":"not found"}'),
        ("GET", "/feed/", 404, b'{"error":"not found"}'),
        ("GET", "/FEED", 404, b'{"error":"not found"}'),
        ("DELETE", "/feed", 404, b'{"error":"not found"}'),
        ("POST", "/feed", 404, b'{"error":"not found"}'),
        ("GET", "/posts/1/like", 404, b'{"error":"not found"}'),
        ("GET", "/posts/007", 200, None),
        ("GET", "/posts/+1", 400, b'{"error":"invalid post id"}'),
        ("GET", "/posts/1e3", 400, b'{"error":"invalid post id"}'),
        ("GET", "/posts/0x10", 400, b'{"error":"invalid post id"}'),
        ("GET", "/posts/99999999999999999999", 400, b'{"error":"invalid post id"}'),
        ("GET", "/posts/18446744073709551615", 400, b'{"error":"invalid post id"}'),
        ("GET", "/posts/999999999999999999", 404, b'{"error":"post not found"}'),
        ("GET", "/", 404, b'{"error":"not found"}'),
        ("GET", "/health/", 404, b'{"error":"not found"}'),
    ]
    for m, p, want_st, want_body in cases:
        st, h, b = request(srv.port, m, p)
        check(st == want_st and (want_body is None or b == want_body), f"{m} {p} -> {want_st}", f"got {st} {b[:80]!r}")
    st, h, b = request(srv.port, "POST", "/posts/abc/like")
    check(st == 401 and b == b'{"error":"missing bearer token"}', "auth before id: no token -> 401")
    st, h, b = request(srv.port, "POST", "/posts/abc/like", {"Authorization": "Bearer " + token()})
    check(st == 400 and b == b'{"error":"invalid post id"}', "auth before id: with token -> 400")
    st, h, b = request(srv.port, "POST", "/posts/abc/like", {"Authorization": "Bearer nope"})
    check(st == 401 and b == b'{"error":"invalid or expired token"}', "auth before id: bad token -> 401")
    st, h, b = request(srv.port, "POST", "/posts", {"Authorization": "Bearer " + token()}, body="")
    check(st == 400 and b == b'{"error":"malformed JSON body"}', "empty request body -> malformed JSON")
    st, h, b = request(srv.port, "POST", "/posts", {"Authorization": "Bearer " + token()}, body="[1,2]")
    check(st == 400 and b == b'{"error":"body is required"}', "array body -> body is required")
    st, h, b = request(srv.port, "POST", "/posts", {"Authorization": "Bearer " + token()}, body='{"body":null}')
    check(st == 400 and b == b'{"error":"body is required"}', "null body -> body is required")
    st, h, b = request(srv.port, "POST", "/posts", {"Authorization": "Bearer " + token()},
                       body='{"body":"\\u00a0 x \\u00a0"}')
    check(st == 201, "non-ASCII whitespace is content, not trimmed", f"{st} {b[:80]!r}")


def test_jwt(srv):
    print("-- JWT edge cases")
    A = lambda t: {"Authorization": "Bearer " + t}
    body = '{"body":"x"}'
    inv = b'{"error":"invalid or expired token"}'
    pay = b'{"error":"invalid token payload"}'
    miss = b'{"error":"missing bearer token"}'
    now = int(time.time())
    base = {"sub": "1", "username": "golden_ember_1", "iat": now, "exp": now + 3600}

    def case(name, tok, want):
        st, h, b = request(srv.port, "POST", "/posts", A(tok), body=body)
        check(st == 401 and b == want, name, f"got {st} {b!r}")

    case("bad signature", token()[:-5] + "AAAAA", inv)
    case("signature of another secret", sign(base, secret="other"), inv)
    case("alg=none, empty signature", b64url(b'{"alg":"none","typ":"JWT"}') + "." + token().split(".")[1] + ".", inv)
    case("alg=none, signed anyway", sign(base, header={"alg": "none"}), inv)
    case("alg=HS512 header with an HS256 signature", sign(base, header={"alg": "HS512", "typ": "JWT"}), inv)
    case("alg=hs256 lowercase", sign(base, header={"alg": "hs256"}), inv)
    case("expired", sign(dict(base, exp=now - 1)), inv)
    case("exp == now", sign(dict(base, exp=now)), inv)
    case("missing exp", sign({k: v for k, v in base.items() if k != "exp"}), inv)
    case("exp as string", sign(dict(base, exp=str(now + 3600))), inv)
    case("exp null", sign(dict(base, exp=None)), inv)
    case("two parts", ".".join(token().split(".")[:2]), inv)
    case("four parts", token() + ".x", inv)
    # "Bearer " with nothing after it: header values are trimmed, so the value
    # is "Bearer", which does not start with "Bearer " (Node trims the same way)
    case("empty token", "", miss)
    case("base64 with padding", token().split(".")[0] + "==." + ".".join(token().split(".")[1:]), inv)
    case("standard base64 chars", token().replace("-", "+").replace("_", "/") if "-" in token() or "_" in token() else "a+b.c/d.e", inv)
    case("payload not JSON", (lambda h, p, s: f"{h}.{b64url(b'not json')}.{s}")(*token().split(".")) , inv)
    case("header not JSON", (lambda h, p, s: f"{b64url(b'junk')}.{p}.{s}")(*token().split(".")), inv)
    case("sub 0", sign(dict(base, sub="0")), pay)
    case("sub negative", sign(dict(base, sub="-1")), pay)
    case("sub float", sign(dict(base, sub="1.5")), pay)
    case("sub numeric not string", sign(dict(base, sub=1)), pay)
    case("sub empty", sign(dict(base, sub="")), pay)
    case("sub missing", sign({k: v for k, v in base.items() if k != "sub"}), pay)
    case("username missing", sign({k: v for k, v in base.items() if k != "username"}), pay)
    case("username number", sign(dict(base, username=5)), pay)
    case("username null", sign(dict(base, username=None)), pay)
    for hdr in ["bearer " + token(), "Bearer" + token(), "Token " + token(), "Basic abc"]:
        st, h, b = request(srv.port, "POST", "/posts", {"Authorization": hdr}, body=body)
        check(st == 401 and b == miss, f"Authorization {hdr[:12]!r}... -> missing bearer token", f"got {st} {b!r}")
    st, h, b = request(srv.port, "POST", "/posts", {"authorization": "Bearer " + token()}, body=body)
    check(st == 201, "lowercase authorization header name is accepted", f"got {st} {b!r}")
    st, h, b = request(srv.port, "POST", "/posts", A(sign(dict(base, exp=now + 3600.9))), body=body)
    check(st == 201, "fractional exp accepted", f"got {st} {b!r}")
    st, h, b = request(srv.port, "POST", "/posts", A(sign(dict(base, username='q"uo\\te'))), body=body)
    check(st == 201 and json.loads(b)["post"]["author"] == 'q"uo\\te', "author with quotes is escaped on output")
    st, h, b = request(srv.port, "POST", "/posts", A(sign(dict(base, sub="999999"))), body=body)
    check(st == 201 and json.loads(b)["post"]["author"] == "golden_ember_1",
          "author comes from the token, not the users table")


def test_create_bodies(srv):
    print("-- create bodies")
    A = {"Authorization": "Bearer " + token()}

    def post(b):
        return request(srv.port, "POST", "/posts", A, body=b)

    st, h, b = post('{"body":"' + "é" * 500 + '"}')
    check(st == 201 and len(json.loads(b)["post"]["body"]) == 500, "500 two-byte characters accepted", f"{st} {b[:60]!r}")
    st, h, b = post('{"body":"' + "é" * 501 + '"}')
    check(st == 400 and b == b'{"error":"body must be at most 500 characters"}', "501 two-byte characters rejected")
    st, h, b = post('{"body":"' + "\\ud83d\\ude00" * 500 + '"}')
    check(st == 201, "500 surrogate-pair emoji accepted", f"{st} {b[:60]!r}")
    st, h, b = post('{"body":"  \\n\\t trimmed \\r\\n "}')
    check(st == 201 and json.loads(b)["post"]["body"] == "trimmed", "escaped whitespace trimmed")
    st, h, b = post('{"body":"a\\u0001b\\"c\\\\d\\/e"}')
    check(st == 201 and b.count(b'"body":"a\\u0001b\\"c\\\\d/e"') == 1, "control chars and quotes escaped on output", b[:120])
    st, h, b = post('{"other":1,"body":"nested ok","extra":{"a":[1,2,{"b":null}]}}')
    check(st == 201 and json.loads(b)["post"]["body"] == "nested ok", "extra members ignored")
    st, h, b = post('{"body":"first","body":"second"}')
    check(st == 201 and json.loads(b)["post"]["body"] == "second", "duplicate key: last wins")
    st, h, b = post('{"body":"x"} trailing')
    check(st == 400 and b == b'{"error":"malformed JSON body"}', "trailing garbage is malformed")
    st, h, b = post('{"body":"\\ud800"}')
    check(st == 400 and b == b'{"error":"malformed JSON body"}', "lone surrogate is malformed")
    st, h, b = post('{"body":"tab\there"}')
    check(st == 400 and b == b'{"error":"malformed JSON body"}', "raw control character is malformed")
    st, h, b = post('{"body":"' + "a" * 500 + '"}')
    check(st == 201, "500 ASCII accepted")
    st, h, b = post('{"body":"' + " " * 700 + 'x' + " " * 700 + '"}')
    check(st == 201 and json.loads(b)["post"]["body"] == "x", "long padding trimmed to a 1-char body")
    st, h, b = post('{"body":"' + "a" * 15000 + '"}')
    check(st == 400 and b == b'{"error":"body must be at most 500 characters"}', "15 KB body rejected as too long")
    st, h, b = request(srv.port, "POST", "/posts", dict(A, **{"Content-Type": "text/plain"}), body='{"body":"ct"}')
    check(st == 201, "content-type is not checked")
    st, h, b = request(srv.port, "POST", "/posts", A, body='{"body":"nolen"}', version="1.1")
    check(st == 201, "create with explicit length")
    # a POST without Content-Length has an empty body
    s = connect(srv.port)
    s.sendall(b"POST /posts HTTP/1.1\r\nAuthorization: Bearer " + token().encode() + b"\r\n\r\n")
    st, h, b, _ = read_response(s)
    check(st == 400 and b == b'{"error":"malformed JSON body"}', "POST without Content-Length -> empty body")
    s.close()


def test_pipelining(srv):
    print("-- pipelining and partial requests")
    s = connect(srv.port)
    reqs = build("GET", "/health") + build("GET", "/posts/1") + build("GET", "/nope")
    s.sendall(reqs)
    buf = b""
    results = []
    for _ in range(3):
        st, h, b, buf = read_response(s, buf)
        results.append(st)
    check(results == [200, 200, 404] and buf == b"", "three pipelined GETs answered in order", str(results))
    # pipelined write then read on the same connection: the read sees the write
    A = {"Authorization": "Bearer " + token()}
    s.sendall(build("POST", "/posts", A, '{"body":"pipelined"}') + build("GET", "/feed"))
    st1, h1, b1, buf = read_response(s)
    st2, h2, b2, buf = read_response(s, buf)
    new_id = json.loads(b1)["post"]["id"]
    check(st1 == 201 and st2 == 200 and json.loads(b2)["posts"][0]["id"] == new_id,
          "pipelined POST then GET /feed: the feed shows the new post")
    s.sendall(build("POST", f"/posts/{new_id}/like", A) + build("GET", f"/posts/{new_id}"))
    st1, h1, b1, buf = read_response(s, buf)
    st2, h2, b2, buf = read_response(s, buf)
    check(st1 == 201 and json.loads(b2)["post"]["like_count"] == 1, "pipelined like then GET sees like_count 1")
    s.close()
    # byte-by-byte
    s = connect(srv.port)
    data = build("POST", "/posts", A, '{"body":"slow"}')
    for i in range(len(data)):
        s.sendall(data[i:i + 1])
        if i % 7 == 0:
            time.sleep(0.002)
    st, h, b, _ = read_response(s)
    check(st == 201 and json.loads(b)["post"]["body"] == "slow", "request sent byte by byte")
    # headers in one packet, body later
    s.sendall(data[:-5])
    time.sleep(0.3)
    s.sendall(data[-5:])
    st, h, b, _ = read_response(s)
    check(st == 201, "body arriving after the headers")
    # two requests, split in the middle of the second's request line
    two = build("GET", "/health") + build("GET", "/posts/2")
    s.sendall(two[:len(two) - 10])
    time.sleep(0.2)
    s.sendall(two[len(two) - 10:])
    st1, h1, b1, buf = read_response(s)
    st2, h2, b2, buf = read_response(s, buf)
    check(st1 == 200 and st2 == 200 and json.loads(b2)["post"]["id"] == 2, "split across a request boundary")
    s.close()
    # Expect: 100-continue
    s = connect(srv.port)
    s.sendall(build("POST", "/posts", dict(A, Expect="100-continue"), None) [:-2] +
              b"Content-Length: 15\r\n\r\n")
    s.settimeout(3)
    got = s.recv(100)
    check(got.startswith(b"HTTP/1.1 100 Continue\r\n\r\n"), "100 Continue sent before the body", got[:60])
    s.sendall(b'{"body":"cont"}')
    st, h, b, _ = read_response(s, got[len(b"HTTP/1.1 100 Continue\r\n\r\n"):])
    check(st == 201 and json.loads(b)["post"]["body"] == "cont", "request completes after 100 Continue")
    s.close()
    # a large pipeline: 200 requests in one write
    s = connect(srv.port)
    s.sendall(build("GET", "/posts/3") * 200)
    buf = b""
    n = 0
    for _ in range(200):
        st, h, b, buf = read_response(s, buf)
        n += st == 200
    check(n == 200, "200 pipelined requests in one write", str(n))
    s.close()


def test_malformed(srv):
    print("-- malformed, oversized and protocol edge cases")

    def raw(data, want_status, name, expect_close=True):
        s = connect(srv.port)
        s.sendall(data)
        try:
            st, h, b, _ = read_response(s)
        except ConnectionError as e:
            bad(name, str(e))
            s.close()
            return
        closed = is_closed(s, 1.0)
        check(st == want_status and (closed or not expect_close) and
              (h.get("connection") == "close" if expect_close else True), name, f"got {st} closed={closed} {h}")
        s.close()

    raw(b"GARBAGE\r\n\r\n", 400, "garbage request line -> 400 and close")
    raw(b"GET /feed HTTP/1.1\r\nBad header\r\n\r\n", 400, "header without colon -> 400")
    raw(b"GET /feed HTTP/2.0\r\n\r\n", 400, "HTTP/2.0 -> 400")
    raw(b"GET /feed HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400, "chunked request -> 400")
    raw(b"GET /feed HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello", 400, "duplicate content-length")
    raw(b"GET /feed HTTP/1.1\r\nContent-Length: abc\r\n\r\n", 400, "non-numeric content-length")
    raw(b"GET /feed HTTP/1.1\r\nX: " + b"a" * 9000 + b"\r\n\r\n", 413, "9 KB header -> 413 and close")
    raw(b"POST /posts HTTP/1.1\r\nContent-Length: 20000\r\n\r\n" + b"x" * 20000, 413, "20 KB body -> 413 and close")
    raw(b"GET /feed HTTP/1.0\r\n\r\n", 200, "HTTP/1.0 closes after the response")
    raw(b"GET /feed HTTP/1.1\r\nConnection: close\r\n\r\n", 200, "Connection: close honoured")
    raw(b"GET /feed HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", 200, "HTTP/1.0 keep-alive stays open", expect_close=False)
    raw(b"GET /feed HTTP/1.1\nHost: x\n\n", 200, "bare LF line endings tolerated", expect_close=False)
    raw(b"GET /feed HTTP/1.1\r\n\r\n", 200, "no headers at all is fine", expect_close=False)
    raw(b"GET  /feed HTTP/1.1\r\n\r\n", 400, "double space in request line -> 400")
    raw(b"get /feed HTTP/1.1\r\n\r\n", 404, "lowercase method is another method -> 404", expect_close=False)
    # a request that is never completed is not answered until the timeout, and
    # the server stays responsive meanwhile
    s = connect(srv.port)
    s.sendall(b"GET /feed HTTP/1.1\r\nX: partial")
    st, h, b = request(srv.port, "GET", "/health")
    check(st == 200, "server responsive while another connection sends a partial request")
    check(not is_closed(s, 0.5), "partial request connection is still open before the timeout")
    s.close()
    # oversized header without the terminator: closed without waiting
    s = connect(srv.port)
    s.sendall(b"GET /feed HTTP/1.1\r\nX: " + b"a" * 9000)
    s.settimeout(5)
    try:
        data = s.recv(200)
        check(data.startswith(b"HTTP/1.1 413"), "unterminated 9 KB header -> 413 at once", data[:40])
    except socket.timeout:
        bad("unterminated 9 KB header -> 413 at once", "timeout")
    s.close()
    # a huge garbage blob
    s = connect(srv.port)
    try:
        s.sendall(b"\x00" * 100000)
    except OSError:
        pass
    s.settimeout(5)
    # the 413 may be lost to the RST that closing a socket with unread data
    # produces; what matters is that the connection ends and the server lives
    try:
        data = s.recv(200)
        check(data == b"" or data.startswith(b"HTTP/1.1 4"), "100 KB of binary -> 4xx or reset", data[:40])
    except socket.timeout:
        bad("100 KB of binary -> 4xx or reset", "timeout")
    except OSError:
        ok("100 KB of binary -> 4xx or reset (reset)")
    s.close()
    st, h, b = request(srv.port, "GET", "/health")
    check(st == 200, "healthy after the binary blob")


def test_keepalive(srv, long):
    print("-- keep-alive")
    s = connect(srv.port)
    for i in range(50):
        st, h, b = request(srv.port, "GET", "/posts/%d" % (i + 1), sock=s)
        if st != 200:
            bad("50 requests on one connection", f"request {i} -> {st}")
            break
    else:
        ok("50 requests on one connection")
    s.close()
    s = connect(srv.port)
    request(srv.port, "GET", "/health", sock=s)
    time.sleep(3)
    st, h, b = request(srv.port, "GET", "/health", sock=s)
    check(st == 200, "connection reused after 3 s idle")
    if long:
        print("   (waiting 66 s for the long keep-alive check)")
        time.sleep(66)
        st, h, b = request(srv.port, "GET", "/health", sock=s)
        check(st == 200, "connection reused after 66 s idle")
    s.close()
    # 500 idle connections, then each is used once
    socks = [connect(srv.port) for _ in range(500)]
    time.sleep(0.5)
    good = 0
    for s in socks:
        st, h, b = request(srv.port, "GET", "/health", sock=s)
        good += st == 200
    check(good == 500, "500 idle keep-alive connections all served", str(good))
    for s in socks:
        s.close()


def test_likes_and_concurrency(srv):
    print("-- likes, idempotency, concurrency")
    A = {"Authorization": "Bearer " + token()}
    st, h, b = request(srv.port, "POST", "/posts", A, body='{"body":"like me"}')
    pid = json.loads(b)["post"]["id"]
    st1, _, b1 = request(srv.port, "POST", f"/posts/{pid}/like", A)
    st2, _, b2 = request(srv.port, "POST", f"/posts/{pid}/like", A)
    st3, _, b3 = request(srv.port, "POST", f"/posts/{pid}/like", A)
    check((st1, st2, st3) == (201, 200, 200) and json.loads(b1)["already_liked"] is False and
          json.loads(b3)["already_liked"] is True, "like is idempotent: 201, 200, 200")
    check(json.loads(request(srv.port, "GET", f"/posts/{pid}")[2])["post"]["like_count"] == 1, "one like counted once")
    check(json.loads(b1) == {"liked": True, "already_liked": False, "post_id": pid}, "like body and key order")

    # 40 users like the same post concurrently, twice each
    def liker(uid, out):
        s = connect(srv.port)
        hdr = {"Authorization": "Bearer " + token(sub=str(uid), username=f"user{uid}")}
        for _ in range(2):
            st, _, _ = request(srv.port, "POST", f"/posts/{pid}/like", hdr, sock=s)
            out.append(st)
        s.close()

    out = []
    threads = [threading.Thread(target=liker, args=(u, out)) for u in range(2, 42)]
    [t.start() for t in threads]
    [t.join() for t in threads]
    cnt = json.loads(request(srv.port, "GET", f"/posts/{pid}")[2])["post"]["like_count"]
    check(sorted(out) == [200] * 40 + [201] * 40 and cnt == 41, "40 concurrent likers: 40x201, 40x200, count 41",
          f"count={cnt} statuses={sorted(set(out))}")

    # 30 threads each create 10 posts; every 201 must be readable and the feed consistent
    created = []
    lock = threading.Lock()

    def creator(uid):
        s = connect(srv.port)
        hdr = {"Authorization": "Bearer " + token(sub=str(uid), username=f"user{uid}")}
        for i in range(10):
            st, _, b = request(srv.port, "POST", "/posts", hdr, body=json.dumps({"body": f"u{uid} p{i}"}), sock=s)
            if st == 201:
                p = json.loads(b)["post"]
                st2, _, b2 = request(srv.port, "GET", f"/posts/{p['id']}", sock=s)
                # the create response names the token's username; GET names users.username
                got = json.loads(b2)["post"] if st2 == 200 else {}
                same = {k: v for k, v in got.items() if k != "author"} == {k: v for k, v in p.items() if k != "author"}
                with lock:
                    created.append((same and p["author"] == f"user{uid}" and got.get("author", "").endswith(f"_{uid}"), p))
            else:
                with lock:
                    created.append((False, None))
        s.close()

    threads = [threading.Thread(target=creator, args=(u,)) for u in range(100, 130)]
    [t.start() for t in threads]
    [t.join() for t in threads]
    check(len(created) == 300 and all(c[0] for c in created), "300 concurrent creates, each read back identical",
          f"{sum(1 for c in created if c[0])}/300")
    ids = [c[1]["id"] for c in created if c[1]]
    check(len(set(ids)) == len(ids), "created ids are unique")
    feed = json.loads(request(srv.port, "GET", "/feed")[2])["posts"]
    keys = [(p["created_at"], p["id"]) for p in feed]
    check(len(feed) == 20 and keys == sorted(keys, reverse=True) and feed[0]["id"] == max(ids),
          "feed after the burst: 20 posts, ordered, newest on top")
    # readers hammering the feed while writers write: every feed is well-formed and ordered
    stop = threading.Event()
    errors = []

    def reader():
        s = connect(srv.port)
        while not stop.is_set():
            st, _, b = request(srv.port, "GET", "/feed", sock=s)
            posts = json.loads(b)["posts"]
            k = [(p["created_at"], p["id"]) for p in posts]
            if st != 200 or len(posts) != 20 or k != sorted(k, reverse=True):
                errors.append(st)
        s.close()

    rs = [threading.Thread(target=reader) for _ in range(4)]
    [t.start() for t in rs]
    ws = [threading.Thread(target=creator, args=(u,)) for u in range(200, 210)]
    [t.start() for t in ws]
    [t.join() for t in ws]
    stop.set()
    [t.join() for t in rs]
    check(not errors, "feeds stay well-formed and ordered during concurrent writes", str(errors[:5]))


def test_churn(srv):
    print("-- connection churn")
    t0 = time.time()
    n = 0
    for i in range(1500):
        st, _, _ = request(srv.port, "GET", "/posts/%d" % (i % 100 + 1))
        n += st == 200
    check(n == 1500, "1500 connect/request/close cycles", f"{n} ok in {time.time() - t0:.1f}s")
    # abrupt closes: connect, send half a request, reset
    for i in range(300):
        s = connect(srv.port)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
        s.sendall(b"GET /feed HT")
        s.close()
    st, _, _ = request(srv.port, "GET", "/health")
    check(st == 200, "healthy after 300 resets mid-request")
    # The server shuts the socket down for a "Connection: close" response, and a
    # client that hangs up at once and reconnects used to get a stale shutdown
    # on the reused descriptor slot (an EOF on its first request, about 8% of
    # these cycles). Every one of them must be answered.
    lost = 0
    for i in range(1000):
        s = connect(srv.port)
        try:
            s.sendall(build("GET", "/health"))
            read_response(s)
            s.sendall(build("GET", "/health", {"Connection": "close"}))
            read_response(s)
        except ConnectionError:
            lost += 1
        s.close()
    check(lost == 0, "1000 close-then-reconnect cycles, none loses a request", f"{lost} lost")


def test_durability(binary, seed):
    print("-- durability: SIGKILL right after a 201")
    srv = Server(binary, seed).start()
    A = {"Authorization": "Bearer " + token()}
    try:
        posts = []
        for i in range(3):
            st, _, b = request(srv.port, "POST", "/posts", A, body=json.dumps({"body": f"survive {i}"}))
            p = json.loads(b)["post"]
            stl, _, _ = request(srv.port, "POST", f"/posts/{p['id']}/like", A)
            posts.append(p)
            srv.kill9()
            srv.start()
        for p in posts:
            st, _, b = request(srv.port, "GET", f"/posts/{p['id']}")
            got = json.loads(b)["post"] if st == 200 else None
            check(got is not None and got["body"] == p["body"] and got["created_at"] == p["created_at"] and
                  got["like_count"] == 1, f"post {p['id']} and its like survived SIGKILL", f"{st} {b[:100]!r}")
        # the WAL file is in use: a restart on the same files must still be healthy
        st, _, b = request(srv.port, "GET", "/health")
        check(st == 200, "healthy after restarts on the same database files")
    finally:
        srv.cleanup()


def test_slowloris(binary, seed):
    print("-- slowloris: partial requests time out")
    srv = Server(binary, seed, ["--partial-timeout-s", "1"]).start()
    try:
        socks = []
        for i in range(100):
            s = connect(srv.port)
            s.sendall(b"GET /feed HTTP/1.1\r\nX-Slow: " + b"a" * (i % 50))
            socks.append(s)
        st, _, _ = request(srv.port, "GET", "/health")
        check(st == 200, "healthy with 100 slowloris connections open")
        time.sleep(13)  # timeout 1 s, sweep every 10 s
        closed = sum(1 for s in socks if is_closed(s, 0.2))
        check(closed == 100, "all 100 partial-request connections closed after the timeout", f"{closed}/100")
        idle = connect(srv.port)
        time.sleep(12)
        st, _, _ = request(srv.port, "GET", "/health", sock=idle)
        check(st == 200, "an idle keep-alive connection without input is not timed out")
        for s in socks:
            s.close()
        idle.close()
    finally:
        srv.cleanup()


def test_slot_exhaustion(binary, seed):
    print("-- connection table exhaustion and multiple rings")
    # 4 rings of 16 slots: 64 connections cross ring boundaries
    srv = Server(binary, seed, ["--max-conns", "64", "--ring-slots", "16"]).start()
    try:
        socks = [connect(srv.port) for _ in range(60)]
        good = sum(1 for s in socks if request(srv.port, "GET", "/health", sock=s)[0] == 200)
        check(good == 60, "60 connections across 4 rings of 16 slots all served", str(good))
        good = sum(1 for s in socks if request(srv.port, "GET", "/feed", sock=s)[0] == 200)
        check(good == 60, "second round on every connection", str(good))
        # beyond the table: extra connections wait in the listen queue, not refused
        extra = [connect(srv.port) for _ in range(20)]
        time.sleep(0.5)
        for s in extra:
            s.sendall(build("GET", "/health"))
        for s in socks[:30]:
            s.close()
        time.sleep(0.5)
        served = 0
        for s in extra:
            s.settimeout(5)
            try:
                st, _, _, _ = read_response(s)
                served += st == 200
            except OSError:
                pass
        check(served == 20, "connections beyond the table are served once slots free up", f"{served}/20")
        for s in socks[30:] + extra:
            s.close()
        time.sleep(0.3)
        st, _, _ = request(srv.port, "GET", "/health")
        check(st == 200, "healthy after the slots were recycled")
        log = srv.logtail()
        check("4 ring(s) x 16 slots" in log, "server reported 4 rings", log[-300:])
    finally:
        srv.cleanup()
    # lowered RLIMIT_NOFILE sizes the rings automatically
    srv = Server(binary, seed, ["--max-conns", "100", "--rlimit-nofile", "40"]).start()
    try:
        socks = [connect(srv.port) for _ in range(90)]
        good = sum(1 for s in socks if request(srv.port, "GET", "/health", sock=s)[0] == 200)
        check(good == 90, "90 connections with RLIMIT_NOFILE=40 (3 rings)", str(good))
        for s in socks:
            s.close()
        log = srv.logtail()
        check("3 ring(s) x 40 slots" in log, "rings derived from the hard limit", log[-300:])
    finally:
        srv.cleanup()


def test_write_hold(binary, seed):
    print("-- WAL growth, checkpoints and write holds under a write burst")
    srv = Server(binary, seed, ["--hold-frames", "300", "--ckpt-frames", "100", "--stats-interval-s", "1"]).start()
    try:
        A = {"Authorization": "Bearer " + token()}
        errors = 0
        lat = []

        def writer(uid):
            nonlocal errors
            s = connect(srv.port)
            hdr = {"Authorization": "Bearer " + token(sub=str(uid), username=f"w{uid}")}
            for i in range(150):
                t0 = time.time()
                st, _, _ = request(srv.port, "POST", "/posts", hdr, body=json.dumps({"body": "x" * 400}), sock=s)
                lat.append(time.time() - t0)
                if st != 201:
                    errors += 1
            s.close()

        ts = [threading.Thread(target=writer, args=(u,)) for u in range(300, 316)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        lat.sort()
        p99 = lat[int(len(lat) * 0.99)]
        check(errors == 0, "2400 creates during holds and checkpoints: no errors", str(errors))
        check(p99 < 1.0, "write p99 under 1 s during holds", f"p99={p99 * 1000:.0f} ms max={lat[-1] * 1000:.0f} ms")
        time.sleep(1.5)
        log = srv.logtail()
        import re
        m = re.findall(r"holds=(\d+)", log)
        ck = re.findall(r"ckpts=(\d+)", log)
        check(m and int(m[-1]) >= 1 and ck and int(ck[-1]) >= 1, "holds and checkpoints happened",
              f"holds={m[-1:] } ckpts={ck[-1:]}")
        st, _, b = request(srv.port, "GET", "/feed")
        check(st == 200 and len(json.loads(b)["posts"]) == 20, "feed fine after the burst")
    finally:
        srv.cleanup()


def test_feed_fallback(binary, seed):
    print("-- feed: reference query and range scans agree")
    srv_ref = Server(binary, seed, ["--feed-reference"]).start()
    srv_rng = Server(binary, seed).start()
    try:
        a = request(srv_ref.port, "GET", "/feed")[2]
        b = request(srv_rng.port, "GET", "/feed")[2]
        check(a == b, "identical feed bytes from both strategies")
        A = {"Authorization": "Bearer " + token()}
        for srv in (srv_ref, srv_rng):
            for i in range(25):
                request(srv.port, "POST", "/posts", A, body=json.dumps({"body": f"same {i}"}))
                if i % 3 == 0:
                    request(srv.port, "POST", f"/posts/{500000 + i}/like", A)
        a = json.loads(request(srv_ref.port, "GET", "/feed")[2])
        b = json.loads(request(srv_rng.port, "GET", "/feed")[2])
        strip = lambda f: [(p["id"], p["body"], p["author"], p["like_count"]) for p in f["posts"]]
        check(strip(a) == strip(b), "identical feeds after writes (ignoring timestamps)")
    finally:
        srv_ref.cleanup()
        srv_rng.cleanup()


def test_shedding(binary, seed):
    print("-- shedding: Connection: close under forced pressure, idle timeout")
    # every response closes
    srv = Server(binary, seed, ["--shed-force", "1"]).start()
    try:
        s = connect(srv.port)
        s.sendall(build("GET", "/health"))
        st, h, b, _ = read_response(s)
        check(st == 200 and h.get("connection") == "close", "shed 1.0: response carries Connection: close", str(h))
        check(is_closed(s, 2.0), "shed 1.0: the server closes after the response")
        s.close()
        # HTTP/1.0 keep-alive request: shed wins over the announced keep-alive
        s = connect(srv.port)
        s.sendall(b"GET /health HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
        st, h, b, _ = read_response(s)
        check(h.get("connection") == "close", "shed 1.0: HTTP/1.0 keep-alive is answered with close", str(h))
        check(is_closed(s, 2.0), "shed 1.0: HTTP/1.0 connection closed after the response")
        s.close()
        # pipelined pair: the first response closes, the second is never served
        s = connect(srv.port)
        s.sendall(build("GET", "/health") + build("GET", "/health"))
        st, h, b, rest = read_response(s)
        check(st == 200 and h.get("connection") == "close" and is_closed(s, 2.0),
              "shed 1.0: pipelined second request is dropped with the close")
        s.close()
    finally:
        srv.cleanup()
    # half of the responses close, deterministically, keep-alive otherwise intact
    srv = Server(binary, seed, ["--shed-force", "0.5"]).start()
    try:
        closes = 0
        s = connect(srv.port)
        for i in range(100):
            s.sendall(build("GET", "/health"))
            st, h, b, _ = read_response(s)
            if h.get("connection") == "close":
                closes += 1
                s.close()
                s = connect(srv.port)
        s.close()
        check(48 <= closes <= 52, "shed 0.5: about half of 100 responses close", str(closes))
        st, h, b = request(srv.port, "GET", "/feed")
        check(st == 200 and len(json.loads(b)["posts"]) == 20, "shed 0.5: feed still correct")
    finally:
        srv.cleanup()
    # no shedding by default on a quiet machine
    srv = Server(binary, seed, []).start()
    try:
        s = connect(srv.port)
        closes = 0
        for i in range(20):
            s.sendall(build("GET", "/health"))
            st, h, b, _ = read_response(s)
            closes += h.get("connection") == "close"
        s.close()
        check(closes == 0, "no shedding below the thresholds")
    finally:
        srv.cleanup()
    # idle timeout: a silent keep-alive is closed, an active one is kept
    srv = Server(binary, seed, ["--idle-timeout-s", "1"]).start()
    try:
        idle = connect(srv.port)
        request(srv.port, "GET", "/health", sock=idle)
        busy = connect(srv.port)
        deadline = time.time() + 6
        closed = False
        while time.time() < deadline and not closed:
            request(srv.port, "GET", "/health", sock=busy)
            time.sleep(0.5)
            closed = is_closed(idle, 0.1)
        check(closed, "idle keep-alive closed by the idle timeout")
        st, h, b = request(srv.port, "GET", "/health", sock=busy)
        check(st == 200, "active keep-alive survives the idle timeout")
        busy.close()
        idle.close()
        log = srv.logtail(20000)
    finally:
        srv.cleanup()


def test_review_regressions(srv):
    print("-- regressions from the review")
    A = {"Authorization": "Bearer " + token()}
    # M1: the feed must fit whatever 20 legal posts contain
    for i in range(8):
        st, _, _ = request(srv.port, "POST", "/posts", A, body=json.dumps({"body": "\U0001F600" * 500}))
        check(st == 201, f"post {i} of 500 emoji -> 201") if i == 0 else None
    st, h, b = request(srv.port, "GET", "/feed")
    check(st == 200 and len(json.loads(b)["posts"]) == 20, "feed with 8 x 500-emoji posts -> 200", f"{st} {b[:80]!r}")
    for i in range(20):
        request(srv.port, "POST", "/posts", A, body=json.dumps({"body": '"' * 500}))
    st, h, b = request(srv.port, "GET", "/feed")
    check(st == 200 and all(p["body"] == '"' * 500 for p in json.loads(b)["posts"]), "feed with 20 x 500 quotes -> 200",
          f"{st} {b[:80]!r}")
    for i in range(20):
        request(srv.port, "POST", "/posts", A, body=json.dumps({"body": "\u0001" * 500}))
    st, h, b = request(srv.port, "GET", "/feed")
    check(st == 200 and len(b) > 20 * 3000, "feed with 20 x 500 control characters (6 bytes each) -> 200", f"{st} {len(b)}")
    st, h, b = request(srv.port, "POST", "/posts", {"Authorization": "Bearer " + token(username="u" * 1000)},
                       body='{"body":"x"}')
    check(st == 201 and json.loads(b)["post"]["author"] == "u" * 1000, "create with a 1000-byte username -> 201")
    # M2: request followed by a half-close in the same batch must still be answered
    results = []
    lock = threading.Lock()

    def half_close():
        s = connect(srv.port)
        s.sendall(build("GET", "/health"))
        s.shutdown(socket.SHUT_WR)
        try:
            st2, h2, b2, _ = read_response(s)
            closed = is_closed(s, 2.0)
            with lock:
                results.append((st2, h2.get("connection"), closed))
        except Exception as e:
            with lock:
                results.append(("err", str(e), False))
        s.close()

    for _ in range(3):
        ts = [threading.Thread(target=half_close) for _ in range(64)]
        [t.start() for t in ts]
        [t.join() for t in ts]
    # the FIN may land in the same batch as the request (answered with
    # Connection: close) or after the response went out (closed on EOF)
    good = sum(1 for r in results if r[0] == 200 and r[2])
    check(good == 192, "192 request+shutdown(SHUT_WR) clients all answered, then closed", str(results[:3]))
    # M3: no 100 Continue for a client that is still sending headers
    s = connect(srv.port)
    s.sendall(build("POST", "/posts", {"Expect": "100-continue", "Content-Length": "2"}))
    s.settimeout(3)
    got = s.recv(100)
    check(got.startswith(b"HTTP/1.1 100 Continue"), "100 Continue for a real Expect request", got[:40])
    s.sendall(b"{}")
    read_response(s, got[len(b"HTTP/1.1 100 Continue\r\n\r\n"):])
    s.close()
    s2 = connect(srv.port)
    s2.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n")
    s2.settimeout(0.7)
    try:
        spurious = s2.recv(100)
    except socket.timeout:
        spurious = b""
    check(spurious == b"", "no spurious 100 Continue while headers are incomplete", spurious[:40])
    s2.sendall(b"\r\n")
    st, _, _, _ = read_response(s2)
    check(st == 200, "the completed request is answered normally")
    s2.close()
    # m1: invalid UTF-8 and NUL never reach the CHECK constraint as a 500
    s = connect(srv.port)
    s.sendall(build("POST", "/posts", A, b'{"body":"' + b"a" * 500 + b'\x80"}'))
    st, _, b, _ = read_response(s)
    check(st == 400 and b == b'{"error":"malformed JSON body"}', "stray continuation byte -> 400", f"{st} {b!r}")
    s.close()
    st, _, b = request(srv.port, "POST", "/posts", A, body='{"body":"\\u0000abc"}')
    check(st == 400 and b == b'{"error":"body is required"}', "NUL in body -> 400", f"{st} {b!r}")
    st, _, b = request(srv.port, "POST", "/posts", A, body='{"body":"ab\\u0000c"}')
    check(st == 400, "embedded NUL -> 400", f"{st} {b!r}")
    s = connect(srv.port)
    s.sendall(build("POST", "/posts", A, b'{"body":"\xf0\x9f\x98"}'))
    st, _, b, _ = read_response(s)
    check(st == 400 and b == b'{"error":"malformed JSON body"}', "truncated 4-byte sequence -> 400", f"{st} {b!r}")
    s.close()
    # m7, m8: HTTP/1.0 keep-alive announced; Date header present
    s = connect(srv.port)
    s.sendall(b"GET /health HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
    st, h, b, _ = read_response(s)
    check(h.get("connection") == "keep-alive", "HTTP/1.0 keep-alive is announced", str(h))
    import re
    check(re.match(r"^[A-Z][a-z]{2}, \d{2} [A-Z][a-z]{2} \d{4} \d{2}:\d{2}:\d{2} GMT$", h.get("date", "")) is not None,
          "Date header present and well-formed", h.get("date"))
    st, h, b, _ = read_response(s) if False else (None, None, None, None)
    s.close()
    st, h, b = request(srv.port, "GET", "/health", version="1.1")
    check("connection" not in h and "date" in h, "HTTP/1.1 keep-alive stays implicit, Date present")
    # n4: query strings do not change the route
    st, _, b = request(srv.port, "GET", "/posts/1?x=1")
    check(st == 200 and json.loads(b)["post"]["id"] == 1, "GET /posts/1?x=1 -> post 1")
    st, _, b = request(srv.port, "GET", "/feed?page=2")
    check(st == 200 and len(json.loads(b)["posts"]) == 20, "GET /feed?page=2 -> feed")
    st, _, b = request(srv.port, "GET", "/health?")
    check(st == 200, "GET /health? -> 200")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--seed", required=True)
    ap.add_argument("--long", action="store_true", help="include the 66 s keep-alive wait")
    ap.add_argument("--only", help="comma-separated test names")
    args = ap.parse_args()
    binary = os.path.abspath(args.server)
    seed = os.path.abspath(args.seed)
    only = set(args.only.split(",")) if args.only else None

    def run(name, fn, *a):
        if only and name not in only:
            return
        try:
            fn(*a)
        except Exception as e:  # noqa
            import traceback
            traceback.print_exc()
            bad(f"{name} raised {type(e).__name__}: {e}")

    srv = Server(binary, seed).start()
    try:
        run("exact", test_exact_bytes, srv)
        run("routing", test_routing, srv)
        run("jwt", test_jwt, srv)
        run("bodies", test_create_bodies, srv)
        run("pipelining", test_pipelining, srv)
        run("malformed", test_malformed, srv)
        run("keepalive", test_keepalive, srv, args.long)
        run("likes", test_likes_and_concurrency, srv)
        run("churn", test_churn, srv)
        run("review", test_review_regressions, srv)
        rc = srv.stop()
        check(rc == 0, "server exits cleanly on SIGTERM", f"rc={rc}\n{srv.logtail()[-500:]}")
    finally:
        srv.cleanup()
    run("durability", test_durability, binary, seed)
    run("slowloris", test_slowloris, binary, seed)
    run("shedding", test_shedding, binary, seed)
    run("slots", test_slot_exhaustion, binary, seed)
    run("hold", test_write_hold, binary, seed)
    run("feed", test_feed_fallback, binary, seed)
    print(f"passed={passed} failed={failed}")
    if failures:
        print("failures:\n  " + "\n  ".join(failures))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
