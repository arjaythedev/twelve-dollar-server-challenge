#!/usr/bin/env python3
"""Extra integration checks for a seeded, running server; do not run during benchmarks.

JWT_SECRET=... python3 check.py [http://127.0.0.1:3000]
Creates posts and likes using TEST_USER_ID (default: 1). Uses Python's standard library.
"""
import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import hmac
import http.client
import json
import os
import sys
import time
from urllib.parse import urlsplit

base = urlsplit(sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:3000")
secret = os.environ.get("JWT_SECRET", "twelve-dollar-challenge").encode()
claims = {"sub": os.environ.get("TEST_USER_ID", "1"), "username": "integration ✓", "exp": int(time.time()) + 3600}
header = {"alg": "HS256", "typ": "JWT"}
checks = 0


def encode(value):
    return value if isinstance(value, bytes) else json.dumps(value, ensure_ascii=False).encode()


def token(payload=claims, head=header):
    b64 = lambda data: base64.urlsafe_b64encode(data).rstrip(b"=")
    signed = b64(encode(head)) + b"." + b64(encode(payload))
    return (signed + b"." + b64(hmac.new(secret, signed, hashlib.sha256).digest())).decode()


auth = token()


def request(method, path, payload=None, jwt=auth):
    connection = (http.client.HTTPSConnection if base.scheme == "https" else http.client.HTTPConnection)(
        base.hostname, base.port, timeout=10)
    try:
        connection.request(method, base.path.rstrip("/") + path,
                           body=None if payload is None else encode(payload),
                           headers={"Authorization": "Bearer " + jwt, "Content-Type": "application/json"})
        response = connection.getresponse()
        return response.status, json.loads(response.read())
    finally:
        connection.close()


def expect(response, status):
    global checks
    assert response[0] == status, ("unexpected HTTP status", status, response)
    checks += 1
    return response[1]


def post(payload, status, body=None, jwt=auth):
    result = expect(request("POST", "/posts", payload, jwt), status)
    if body is not None:
        assert result["post"]["body"] == body, result
    return result


for raw in [b'{body:"x"}', b'{"body":"x",}', b'{"body":NaN}', b'{"body":/*x*/"x"}',
            b"{'body':'x'}", b'{"body":"x"}\x00junk', b'{"body":"\xc0\x80"}',
            b'{"body":"\xed\xa0\x80"}', b'{"body":"\xf4\x90\x80\x80"}']:
    try:
        post(raw, 400)
    except AssertionError as error:
        raise AssertionError((raw, error.args)) from error
for cp in [0x1f600, 0x10000, 0x40000, 0x50000, 0x100000, 0x10ffff]:
    value = chr(cp) * 500
    post(json.dumps({"body": value}).encode(), 201, value)  # Exercise JSON surrogate escapes.
    post({"body": value + chr(cp)}, 400)
spaces = "".join(map(chr, [9, 10, 11, 12, 13, 32, 0xa0, 0x1680, *range(0x2000, 0x200b),
                            0x2028, 0x2029, 0x202f, 0x205f, 0x3000, 0xfeff]))
post({"body": spaces + "edge" + spaces}, 201, "edge")
post({"body": spaces}, 400)
for cp in [0x85, 0x180e, 0x200b]:
    value = chr(cp) + "edge" + chr(cp)
    post({"body": value}, 201, value)
for value in [None, [], {}, 1, True, "x"]:
    post(encode(value), 400)
post(b'{"body":"abc\\u0000def"}', 201, "abc\0def")
print("  ok   strict JSON, Unicode boundaries, trimming and embedded NUL")

value = "😀" * 500
created = post({"body": " \n" + value + "\t "}, 201, value)["post"]
assert created["author"] == claims["username"] and isinstance(created["id"], int) and created["id"] > 0
path = f'/posts/{created["id"]}'
before = expect(request("GET", path), 200)["post"]
assert before["body"] == value and before["like_count"] == 0
with ThreadPoolExecutor(max_workers=16) as pool:
    duplicates = list(pool.map(lambda _: request("POST", path + "/like"), range(16)))
assert sum(status == 201 for status, _ in duplicates) == 1, duplicates
for status, data in duplicates:
    assert status in (200, 201) and data == {"liked": True, "already_liked": status == 200, "post_id": created["id"]}
after = expect(request("GET", path), 200)["post"]
assert after["like_count"] == 1
feed = expect(request("GET", "/feed"), 200)["posts"]
assert next(p for p in feed if p["id"] == created["id"])["like_count"] == 1
print("  ok   16 concurrent duplicate likes insert one row and update reads")

invalid_tokens = [token(head={"alg": "HS512"}), token(head={"alg": "hs256"}), token(head={"alg": "Hs256"}), token(head=None), token({**claims, "exp": str(claims["exp"])}),
                  token({**claims, "exp": 0}), token({**claims, "nbf": time.time() + 3600}),
                  token({**claims, "sub": 1}), token({**claims, "username": None}), token(b"{"),
                  token(b'{"sub":"1","username":"\xc3(","exp":' + str(claims["exp"]).encode() + b'}'),
                  token(head=b'{"alg":"HS256","x":"\xc3("}')]
for jwt in invalid_tokens:
    post({"body": "must not be inserted"}, 401, jwt=jwt)
print("  ok   signed invalid algorithms, claims, JSON and UTF-8 JWT segments")

expiry = time.time() + 2
short_lived = token({**claims, "exp": expiry})
expect(request("POST", path + "/like", jwt=short_lived), 200)
time.sleep(max(0, expiry - time.time() + 0.1))
expect(request("POST", path + "/like", jwt=short_lived), 401)
print("  ok   previously accepted JWT is rejected after expiry")

with ThreadPoolExecutor(max_workers=12) as pool:
    burst = list(pool.map(lambda i: request("POST", "/posts", {"body": f"ordering {i}: {time.time_ns()}"}), range(12)))
posts = [expect(response, 201)["post"] for response in burst]
expected = sorted(posts, key=lambda p: (p["created_at"], p["id"]), reverse=True)
latest = expect(request("GET", "/feed"), 200)["posts"]
assert [p["id"] for p in latest[:len(posts)]] == [p["id"] for p in expected]
print(f"  ok   feed reflects concurrent creates in timestamp/id order")
print(f"Extra checks passed ({checks} HTTP checks plus 16 concurrent likes). Post {created['id']} contains 500 emoji and one like.")

for jwt in [token(b'{"sub":"1","username":"x","exp":1e999}'),
            token(b'{"sub":"1","username":"x","exp":9999999999,"nbf":1e999}')]:
    post({"body":"invalid numeric date"}, 401, jwt=jwt)
expect(request("POST", "/posts", b"{bad", jwt="bad"), 401)
expect(request("POST", "/posts/abc/like", b"{bad", jwt="bad"), 401)
expect(request("GET", "/posts/000500000"), 200)
expect(request("GET", "/feed.json"), 404)
print(f"Rails extra checks passed ({checks} HTTP checks plus 16 concurrent likes).")
