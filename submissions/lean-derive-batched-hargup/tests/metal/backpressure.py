#!/usr/bin/env python3
"""backpressure.py — a client with a tiny receive buffer pipelines 300 feed requests and reads
nothing for 2 s, so the server's sends cannot complete; meanwhile another connection must still
be answered promptly. Then all 300 answers must arrive complete and in order."""
import json, os, socket, threading, time
HOST, PORT = os.environ.get('HOST', '127.0.0.1'), int(os.environ.get('PORT', '3000'))
def resp(s, buf):
    while b'\r\n\r\n' not in buf:
        part = s.recv(65536)
        if not part: raise EOFError
        buf += part
    head, body = buf.split(b'\r\n\r\n', 1)
    n = int(next(l.split(b':', 1)[1] for l in head.split(b'\r\n') if l.lower().startswith(b'content-length:')))
    while len(body) < n:
        part = s.recv(65536)
        if not part: raise EOFError
        body += part
    return int(head.split()[1]), body[:n], body[n:]
slow = socket.socket()
slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
slow.connect((HOST, PORT))
slow.settimeout(60)
threading.Thread(target=slow.sendall, args=(b'GET /feed HTTP/1.1\r\nHost: x\r\n\r\n' * 300,), daemon=True).start()
time.sleep(0.3)
fast = socket.create_connection((HOST, PORT), timeout=5)
worst, buf = 0.0, b''
for _ in range(50):
    t = time.monotonic()
    fast.sendall(b'GET /posts/1 HTTP/1.1\r\nHost: x\r\n\r\n')
    st, _, buf = resp(fast, buf)
    worst = max(worst, time.monotonic() - t)
    assert st == 200
time.sleep(2)
buf, first = b'', None
for i in range(300):
    st, body, buf = resp(slow, buf)
    posts = json.loads(body)['posts']
    assert st == 200 and len(posts) == 20
    first = first or posts
    assert posts == first   # read-only: every feed answer identical
assert worst < 1.0, worst
print(f'backpressure: 300 pipelined feeds behind a 4 KB receive buffer arrived complete; other client worst {worst*1000:.1f} ms')
