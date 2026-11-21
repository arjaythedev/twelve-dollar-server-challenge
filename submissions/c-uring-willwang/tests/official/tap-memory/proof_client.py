"""Byte-checked private IPv4 connections for the scaled outer-budget proof."""

import base64
import hashlib
import hmac
import http.client
import json
import os
import resource
import socket
import sys
import time

soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
resource.setrlimit(resource.RLIMIT_NOFILE, (max(soft, 4096), hard))
COUNT = 0
HOST = "198.18.203.1"
PORT = 19493
POST = b'{"post":{"id":1,"body":"memory probe","created_at":"2026-11-01T00:00:00.000Z","author":"reader","like_count":0}}'
peers = []


def check(c):
    global COUNT
    c.request("GET", "/posts/1")
    res = c.getresponse()
    data = res.read()
    if (
        res.status != 200
        or data != POST
        or c.sock.getsockopt(socket.SOL_SOCKET, socket.SO_INCOMING_CPU) != 9
    ):
        raise RuntimeError((res.status, data))
    COUNT += 1


def run_op(op, arg):
    global COUNT
    if op == "resize":
        while len(peers) > arg:
            peers.pop().close()
        while len(peers) < arg:
            c = http.client.HTTPConnection(HOST, PORT, timeout=4)
            check(c)
            peers.append(c)
    elif op == "sample":
        for c in peers[:: max(1, len(peers) // 32)]:
            check(c)
    elif op == "write":
        enc = lambda b: base64.urlsafe_b64encode(b).rstrip(b"=")
        j = b".".join(
            [
                enc(b'{"alg":"HS256","typ":"JWT"}'),
                enc(json.dumps(dict(sub="1", username="reader", exp=int(time.time()) + 600)).encode()),
            ]
        )
        token = (j + b"." + enc(hmac.digest(b"twelve-dollar-challenge", j, "sha256"))).decode()
        c = peers[0]
        c.request(
            "POST",
            "/posts",
            body=b'{"body":"private write"}',
            headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"},
        )
        r = c.getresponse()
        post = json.loads(r.read())
        assert r.status == 201 and post["post"]["body"] == "private write"
        c.request("GET", "/posts/" + str(post["post"]["id"]))
        r = c.getresponse()
        assert r.status == 200 and json.loads(r.read()) == post
        COUNT += 2
    print(
        json.dumps(
            dict(
                op=op, peers=len(peers), checked=COUNT, pid=os.getpid(), cpu=sorted(os.sched_getaffinity(0))
            )
        ),
        flush=True,
    )


print("READY", flush=True)
for raw in sys.stdin:
    words = raw.strip().split()
    if not words:
        continue
    if words[0] == "quit":
        break
    run_op(words[0], int(words[1]) if len(words) > 1 else 0)
for conn in peers:
    conn.close()
