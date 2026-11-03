#!/usr/bin/env python3
"""Independent latency probe: one keep-alive connection, one request every
`interval` seconds, so server stalls can be told apart from load-generator
queueing. Prints p50/p99/max at the end (or when the parent kills it).

    tools/probe.py PORT DURATION [interval] [path]
"""
import signal
import socket
import sys
import time

port = int(sys.argv[1])
duration = float(sys.argv[2])
interval = float(sys.argv[3]) if len(sys.argv) > 3 else 0.05
path = sys.argv[4] if len(sys.argv) > 4 else "/feed"
lat = []
errors = 0
stalls = []  # (time, ms) for samples over 100 ms


def report(*_):
    lat.sort()
    n = len(lat)
    if n:
        print("   probe %s: n=%d errors=%d p50=%.1fms p99=%.1fms max=%.1fms over100ms=%d%s" % (
            path, n, errors, lat[n // 2] * 1e3, lat[min(n - 1, int(n * 0.99))] * 1e3, lat[-1] * 1e3, len(stalls),
            (" first=" + ",".join("%.0fs:%.0fms" % s for s in stalls[:6])) if stalls else ""))
    else:
        print("   probe: no samples, errors=%d" % errors)
    sys.stdout.flush()
    sys.exit(0)


signal.signal(signal.SIGTERM, report)
t_end = time.time() + duration
t0 = time.time()
s = None
req = ("GET %s HTTP/1.1\r\nHost: x\r\n\r\n" % path).encode()
while time.time() < t_end:
    try:
        if s is None:
            s = socket.create_connection(("127.0.0.1", port), timeout=10)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        t = time.time()
        s.sendall(req)
        buf = b""
        while b"\r\n\r\n" not in buf:
            d = s.recv(65536)
            if not d:
                raise ConnectionError("eof")
            buf += d
        head, rest = buf.split(b"\r\n\r\n", 1)
        n = int([h for h in head.split(b"\r\n") if h.lower().startswith(b"content-length")][0].split(b":")[1])
        while len(rest) < n:
            d = s.recv(65536)
            if not d:
                raise ConnectionError("eof")
            rest += d
        dt = time.time() - t
        lat.append(dt)
        if dt > 0.1:
            stalls.append((t - t0, dt * 1e3))
    except OSError:
        errors += 1
        try:
            s.close()
        except Exception:
            pass
        s = None
        time.sleep(0.2)
    time.sleep(interval)
report()
