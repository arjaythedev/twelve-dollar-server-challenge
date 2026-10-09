#!/usr/bin/env python3
"""Check completed responses, fragmented uploads and keep-alive recovery under pressure.

Start the server on a fresh seed with hard/soft nofile=256, then run this script
with the server port. The client needs a higher descriptor limit than the server.
"""
import base64
from contextlib import closing as close_after
import hashlib
import hmac
import http.client
import json
import os
import sys
import time

port = int(sys.argv[1])
encode = lambda value: base64.urlsafe_b64encode(json.dumps(value).encode()).rstrip(b"=")
signed = encode({"alg": "HS256"}) + b"." + encode(
    {"sub": "1", "username": "pressure", "exp": time.time() + 3600})
secret = os.getenv("JWT_SECRET", "twelve-dollar-challenge").encode()
token = (signed + b"." + base64.urlsafe_b64encode(
    hmac.new(secret, signed, hashlib.sha256).digest()).rstrip(b"=")).decode()
auth = {"Authorization": "Bearer " + token, "Content-Type": "application/json"}
fillers = []
partial = http.client.HTTPConnection("127.0.0.1", port, timeout=5)


def complete(method, path, body=None):
    with close_after(http.client.HTTPConnection("127.0.0.1", port, timeout=5)) as connection:
        connection.request(method, path, None if body is None else json.dumps(body), auth)
        response = connection.getresponse()
        status, closing = response.status, response.getheader("Connection", "").lower() == "close"
        payload = json.loads(response.read())
        return status, payload, closing


try:
    text = "fragmented under connection pressure " + "🙂" * 200
    payload = json.dumps({"body": text}, ensure_ascii=False).encode()
    partial.putrequest("POST", "/posts")
    for name, value in auth.items():
        partial.putheader(name, value)
    partial.putheader("Content-Length", str(len(payload)))
    partial.endheaders()
    midpoint = len(payload) // 2
    partial.send(payload[:midpoint])
    for _ in range(160):
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        fillers.append(connection)
        connection.request("GET", "/health")
        response = connection.getresponse()
        assert response.status == 200
        response.read()
    time.sleep(.3)  # Allow the Erlang connection-count sampler to observe the held sockets.

    retired = 0
    for index in range(32):
        body = f"committed pressure write {index}"
        status, stored, closing = complete("POST", "/posts", {"body": body})
        assert status == 201, (status, stored)
        retired += closing
        status, read, closing = complete("GET", f'/posts/{stored["post"]["id"]}')
        assert status == 200 and read["post"]["body"] == body, (status, read)
        retired += closing
    assert retired > 0, "pressure path was not exercised; start the server with nofile=256"

    partial.send(payload[midpoint:])
    response = partial.getresponse()
    stored = json.loads(response.read())
    assert response.status == 201 and stored["post"]["body"] == text, stored
    partial.close()
    for connection in fillers:
        connection.close()
    time.sleep(.3)

    with close_after(http.client.HTTPConnection("127.0.0.1", port, timeout=5)) as connection:
        connection.request("GET", "/health")
        response = connection.getresponse()
        assert response.status == 200 and response.getheader("Connection", "").lower() != "close"
        response.read()
        original = connection.sock
        connection.request("GET", "/feed")
        response = connection.getresponse()
        assert response.status == 200
        response.read()
        assert connection.sock is original, "keep-alive did not recover after pressure subsided"
    print(f"PASS {retired} retired connections, 32 committed writes/readbacks, fragmented upload and keep-alive recovery")
finally:
    partial.close()
    for connection in fillers:
        connection.close()
