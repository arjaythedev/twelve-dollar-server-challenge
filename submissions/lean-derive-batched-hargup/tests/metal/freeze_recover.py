#!/usr/bin/env python3
"""freeze_recover.py SEED_DB SERVER_PID [connections] — the receive path under a stall.
Every connection gets one warm-up answer (so its multishot receive is armed); the server is then
stopped (SIGSTOP) while every connection sends a request (1 in 4 padded past a receive buffer,
1 in 5 two pipelined requests) and 1 in 8 connections close right after sending; the server is
resumed. With more pending connections than receive buffers, multishot receives end with ENOBUFS
and the bounded read queue does the reading. Every answer on a live connection must match SQL in
order; then fresh connections (reusing the closed slots) must work."""
import json, os, random, signal, socket, sqlite3, sys, time
seed, pid = sys.argv[1], int(sys.argv[2])
N = int(sys.argv[3]) if len(sys.argv) > 3 else 16000
HOST, PORT = os.environ.get('HOST', '127.0.0.1'), int(os.environ.get('PORT', '3000'))
fields = ['id', 'body', 'created_at', 'author', 'like_count']
q = '''SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id=p.id)
       FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?'''
db = sqlite3.connect('file:' + os.path.abspath(seed) + '?mode=ro', uri=True)
rng = random.Random(11)

def resp(s, buf):
    while b'\r\n\r\n' not in buf:
        part = s.recv(65536)
        if not part: raise EOFError('EOF')
        buf += part
    head, body = buf.split(b'\r\n\r\n', 1)
    n = int(next(l.split(b':', 1)[1] for l in head.split(b'\r\n') if l.lower().startswith(b'content-length:')))
    while len(body) < n:
        part = s.recv(65536)
        if not part: raise EOFError('truncated')
        body += part
    return int(head.split()[1]), body[:n], body[n:]

def req(pid_, pad):
    return f'GET /posts/{pid_} HTTP/1.1\r\nHost: x\r\nX-Pad: {"p" * pad}\r\n\r\n'.encode()

def connect_all(n):
    socks = []
    for _ in range(n):
        s = socket.create_connection((HOST, PORT), timeout=60)
        socks.append(s)
    for s in socks: s.sendall(b'GET /health HTTP/1.1\r\nHost: x\r\n\r\n')
    for s in socks:
        st, _, rest = resp(s, b'')
        assert st == 200 and not rest
    return socks

socks = connect_all(N)
plan, expected = [], {}
for i in range(N):
    ids = [rng.randint(1, 500000) for _ in range(2 if i % 5 == 0 else 1)]
    pads = [2100 if (i + k) % 4 == 0 else 0 for k in range(len(ids))]
    plan.append((ids, pads, i % 8 == 7))
    for x in ids:
        if x not in expected:
            expected[x] = dict(zip(fields, db.execute(q, (x,)).fetchone()))
os.kill(pid, signal.SIGSTOP)
try:
    for s, (ids, pads, closes) in zip(socks, plan):
        s.sendall(b''.join(req(x, p) for x, p in zip(ids, pads)))
        if closes: s.close()
    time.sleep(0.5)
finally:
    os.kill(pid, signal.SIGCONT)
checked = 0
for s, (ids, pads, closes) in zip(socks, plan):
    if closes: continue
    buf = b''
    for x in ids:
        st, body, buf = resp(s, buf)
        post = json.loads(body)['post']
        assert st == 200 and list(post) == fields and post == expected[x], (x, st, body[:80])
        checked += 1
    s.close()
# fresh connections, reusing the slots of the closed ones
fresh = connect_all(2000)
for s in fresh:
    s.sendall(req(1, 0))
    st, body, _ = resp(s, b'')
    assert st == 200 and json.loads(body)['post'] == dict(zip(fields, db.execute(q, (1,)).fetchone()))
    s.close()
print(f'freeze recover: {N} connections, {checked} answers matched SQL after SIGSTOP/SIGCONT, '
      f'{sum(p[2] for p in plan)} closed mid-stall, 2000 fresh connections answered')
