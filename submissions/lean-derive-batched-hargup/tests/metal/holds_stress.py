#!/usr/bin/env python3
"""holds_stress.py LIVE_DB [connections] — writes pipelined behind reads hold their connection
until the batch commits, carried over from pass to pass. Each connection (its own user and post)
pipelines like, read, like, read; the answers must be 201/200 with the right already_liked, and
each read must show the like committed before it (like_count = before + 1)."""
import base64, hashlib, hmac, json, os, socket, sqlite3, sys, threading, time
db_path = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 1500
HOST, PORT = os.environ.get('HOST', '127.0.0.1'), int(os.environ.get('PORT', '3000'))
SECRET = b'twelve-dollar-challenge'
def b64(b): return base64.urlsafe_b64encode(b).rstrip(b'=')
def token(uid):
    m = b64(b'{"alg":"HS256","typ":"JWT"}') + b'.' + b64(json.dumps({'sub': str(uid), 'username': f'h{uid}', 'exp': int(time.time()) + 3600}).encode())
    return (m + b'.' + b64(hmac.new(SECRET, m, hashlib.sha256).digest())).decode()
db = sqlite3.connect('file:' + os.path.abspath(db_path) + '?mode=ro', uri=True)
cases = []
for i in range(N):
    uid, pid = 40001 + i, 420001 + i
    assert not db.execute('SELECT 1 FROM likes WHERE user_id=? AND post_id=?', (uid, pid)).fetchone()
    cases.append((uid, pid, db.execute('SELECT count(*) FROM likes WHERE post_id=?', (pid,)).fetchone()[0]))
db.close()
errors = []
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
    return int(head.split()[1]), json.loads(body[:n]), body[n:]
def run(chunk):
    for uid, pid, base in chunk:
        try:
            s = socket.create_connection((HOST, PORT), timeout=60)
            like = f'POST /posts/{pid}/like HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer {token(uid)}\r\nContent-Length: 0\r\n\r\n'
            get = f'GET /posts/{pid} HTTP/1.1\r\nHost: x\r\n\r\n'
            s.sendall((like + get + like + get).encode())
            buf = b''
            st, o, buf = resp(s, buf); assert st == 201 and o == {'liked': True, 'already_liked': False, 'post_id': pid}, (st, o)
            st, o, buf = resp(s, buf); assert st == 200 and o['post']['like_count'] == base + 1, (st, o)
            st, o, buf = resp(s, buf); assert st == 200 and o == {'liked': True, 'already_liked': True, 'post_id': pid}, (st, o)
            st, o, buf = resp(s, buf); assert st == 200 and o['post']['like_count'] == base + 1, (st, o)
            s.close()
        except Exception as e:  # noqa: BLE001
            errors.append(f'{uid}/{pid}: {e!r}')
ts = [threading.Thread(target=run, args=(cases[k::300],)) for k in range(300)]
for t in ts: t.start()
for t in ts: t.join()
if errors:
    print(f'holds stress: {len(errors)} failed, e.g. {errors[:3]}'); sys.exit(1)
print(f'holds stress: {N} connections x (like, read, like, read) pipelined, statuses and counts correct')
