#!/usr/bin/env python3
"""A raw-socket burst exhausts server descriptors before its next pressure sample.

Start a fresh server with hard/soft nofile=256; the client needs a higher limit.
"""
import json
import http.client
import socket
import sys
import time

port = int(sys.argv[1])
sockets = []
try:
    # Warm the health handler while the descriptor sampler still returns no pressure.
    warm = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    warm.request("GET", "/health")
    warm.getresponse().read()
    warm.close()
    for _ in range(300):
        client = socket.create_connection(("127.0.0.1", port), timeout=5)
        sockets.append(client)
        client.sendall(b"GET /health HTTP/1.1\r\nHost:")
    time.sleep(.3)
    probe = sockets[0]
    probe.sendall(b" localhost\r\n\r\n")
    with probe.makefile("rb") as stream:
        line = stream.readline()
        assert line.startswith(b"HTTP/1.1 200 "), line
        headers = {}
        while (line := stream.readline()) != b"\r\n":
            assert line, "EOF before response headers completed"
            name, value = line.decode().split(":", 1)
            headers[name.lower()] = value.strip()
        assert headers.get("connection", "").lower() == "close", headers
        body = stream.read(int(headers["content-length"]))
        assert json.loads(body)["db"] == "ok", body
        assert stream.read(1) == b"", "server did not drain and close the response"
    print("PASS response drains and closes after a descriptor-exhausting raw-socket burst")
finally:
    for client in sockets: client.close()
