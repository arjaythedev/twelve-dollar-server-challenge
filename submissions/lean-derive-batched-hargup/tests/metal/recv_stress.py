#!/usr/bin/env python3
"""Receive-path stress: many connections each pipeline a burst of requests padded past the
receive buffer size, so the provided-buffer ring runs dry (multishot receives end with
ENOBUFS) and requests are read back through the bounded read queue (Feed.Transport). Every
response is checked against SQL on the seed, in order, per connection.
  recv_stress.py SEED_DB [connections] [requests_per_connection]"""
import json, os, random, socket, sqlite3, sys, threading

seed = sys.argv[1]
conns = int(sys.argv[2]) if len(sys.argv) > 2 else 300
per = int(sys.argv[3]) if len(sys.argv) > 3 else 48
HOST, PORT = os.environ.get('HOST', '127.0.0.1'), int(os.environ.get('PORT', '3000'))
fields = ['id', 'body', 'created_at', 'author', 'like_count']
q = '''SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id=p.id)
       FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?'''
db = sqlite3.connect('file:' + os.path.abspath(seed) + '?mode=ro', uri=True)
largest = db.execute('SELECT max(id) FROM posts').fetchone()[0]
rng = random.Random(7)
plans = []
for c in range(conns):
    reqs = []
    for k in range(per):
        pid = rng.randint(1, largest)
        pad = rng.choice([0, 0, 100, 1500, 2100, 4000, 7000])   # some requests span several buffers
        reqs.append((pid, pad))
    plans.append(reqs)
expected = {pid: dict(zip(fields, db.execute(q, (pid,)).fetchone())) for reqs in plans for pid, _ in reqs}
db.close()
errors = []

def run(i):
    try:
        s = socket.create_connection((HOST, PORT), timeout=60)
        data = b''.join(f'GET /posts/{pid} HTTP/1.1\r\nHost: x\r\nX-Pad: {"p" * pad}\r\n\r\n'.encode()
                        for pid, pad in plans[i])
        threading.Thread(target=s.sendall, args=(data,), daemon=True).start()
        buf = b''
        for pid, _ in plans[i]:
            while b'\r\n\r\n' not in buf:
                part = s.recv(65536)
                assert part, 'EOF'
                buf += part
            head, body = buf.split(b'\r\n\r\n', 1)
            assert head.startswith(b'HTTP/1.1 200 '), head[:40]
            n = int(next(l.split(b':', 1)[1] for l in head.split(b'\r\n') if l.lower().startswith(b'content-length:')))
            while len(body) < n:
                part = s.recv(65536)
                assert part, 'truncated'
                body += part
            buf = body[n:]
            post = json.loads(body[:n])['post']
            assert list(post) == fields and post == expected[pid], (pid, post)
        s.close()
    except Exception as e:  # noqa: BLE001
        errors.append(f'conn {i}: {e!r}')

ts = [threading.Thread(target=run, args=(i,)) for i in range(conns)]
for t in ts: t.start()
for t in ts: t.join()
if errors:
    print(f'recv stress: {len(errors)} connections failed, e.g. {errors[:3]}')
    sys.exit(1)
print(f'recv stress: {conns} connections x {per} pipelined padded requests matched SQL')
