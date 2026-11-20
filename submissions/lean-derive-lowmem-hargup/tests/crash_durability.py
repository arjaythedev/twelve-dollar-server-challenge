#!/usr/bin/env python3
"""Kill -9 the server under pipelined writes, then check the recovered database.

Every acknowledged write (201 post, 200/201 like) must be present after the crash, and
PRAGMA integrity_check must pass. Uses a fresh copy of the seed per round.

usage: crash_durability.py <server-binary> <seed.db> [rounds]
Build the server with a small page cache (e.g. -DDB_CACHE_KIB=64) so large batches spill
pages to the WAL and rewrite frames in place before they commit."""
import base64, hashlib, hmac, json, os, random, shutil, signal, socket, sqlite3, subprocess, sys, tempfile, threading, time

SECRET = b'twelve-dollar-challenge'
PORT = int(os.environ.get('PORT', '3100'))
WORKERS, PIPELINE = 16, 32

def b64(b):
    return base64.urlsafe_b64encode(b).rstrip(b'=')

def token(uid):
    msg = b64(b'{"alg":"HS256","typ":"JWT"}') + b'.' + b64(json.dumps(
        {'sub': str(uid), 'username': f'crash_{uid}', 'iat': 0, 'exp': int(time.time()) + 3600}).encode())
    return (msg + b'.' + b64(hmac.new(SECRET, msg, hashlib.sha256).digest())).decode()

def read_response(s, buf):
    while b'\r\n\r\n' not in buf:
        part = s.recv(65536)
        if not part: raise EOFError
        buf += part
    head, rest = buf.split(b'\r\n\r\n', 1)
    status = int(head.split(b' ', 2)[1])
    n = int([l for l in head.split(b'\r\n') if l.lower().startswith(b'content-length:')][0].split(b':')[1])
    while len(rest) < n:
        part = s.recv(65536)
        if not part: raise EOFError
        rest += part
    return status, json.loads(rest[:n]), rest[n:]

def worker(w, stop, acked_posts, acked_likes, max_post):
    rnd = random.Random(w)
    try:
        s = socket.create_connection(('127.0.0.1', PORT), timeout=10)
    except OSError:
        return
    buf, i = b'', 0
    try:
        while not stop.is_set():
            sent = []
            for _ in range(PIPELINE):
                uid = rnd.randint(1, 50000)
                auth = f'Authorization: Bearer {token(uid)}\r\n'
                if rnd.random() < 0.5:
                    body = json.dumps({'body': f'crash {w}-{i} ' + 'x' * rnd.randint(1, 400)})
                    req = f'POST /posts HTTP/1.1\r\nHost: t\r\n{auth}Content-Type: application/json\r\nContent-Length: {len(body)}\r\n\r\n{body}'
                    sent.append(('post', json.loads(body)['body']))
                else:
                    pid = rnd.randint(1, max_post)
                    req = f'POST /posts/{pid}/like HTTP/1.1\r\nHost: t\r\n{auth}Content-Length: 0\r\n\r\n'
                    sent.append(('like', (uid, pid)))
                i += 1
                s.sendall(req.encode())
            for kind, what in sent:
                status, obj, buf = read_response(s, buf)
                if kind == 'post' and status == 201:
                    acked_posts.append((obj['post']['id'], what))
                elif kind == 'like' and status in (200, 201):
                    acked_likes.append(what)
    except (OSError, EOFError, ValueError):
        pass

def one_round(binary, seed, rnd):
    d = tempfile.mkdtemp(prefix='crash-')
    db = os.path.join(d, 'feed.db')
    shutil.copyfile(seed, db)
    env = dict(os.environ, SQLITE_PATH=db, JWT_SECRET=SECRET.decode(), HOST='127.0.0.1', PORT=str(PORT),
               WALBUF_TRACE=os.path.join(d, 'walbuf.trace'))
    p = subprocess.Popen([binary], env=env, stdout=subprocess.DEVNULL, stderr=open(os.path.join(d, 'server.log'), 'w'))
    for _ in range(200):
        try:
            socket.create_connection(('127.0.0.1', PORT), timeout=1).close(); break
        except OSError:
            time.sleep(0.05)
    stop, acked_posts, acked_likes = threading.Event(), [], []
    ts = [threading.Thread(target=worker, args=(w, stop, acked_posts, acked_likes, 500000)) for w in range(WORKERS)]
    for t in ts: t.start()
    time.sleep(rnd.uniform(0.3, 2.5))
    os.kill(p.pid, signal.SIGKILL)
    p.wait()
    stop.set()
    for t in ts: t.join()
    os.mkdir(os.path.join(d, 'snap'))
    for suffix in ('', '-wal', '-shm'):
        if os.path.exists(db + suffix):
            shutil.copyfile(db + suffix, os.path.join(d, 'snap', 'feed.db' + suffix))
    con = sqlite3.connect(db)
    try:
        ok = con.execute('PRAGMA integrity_check').fetchone()[0]
        missing_posts = sum(1 for pid, body in acked_posts
                            if con.execute('SELECT body FROM posts WHERE id=?', (pid,)).fetchone() != (body,))
        missing_likes = sum(1 for uid, pid in acked_likes
                            if not con.execute('SELECT 1 FROM likes WHERE user_id=? AND post_id=?', (uid, pid)).fetchone())
    except sqlite3.DatabaseError as e:
        ok, missing_posts, missing_likes = f'error: {e}', -1, -1
    con.close()
    if ok == 'ok' and not missing_posts and not missing_likes:
        shutil.rmtree(d)
    else:
        ok += f' (kept {d})'
    return ok, len(acked_posts), len(acked_likes), missing_posts, missing_likes

def main():
    binary, seed = sys.argv[1], sys.argv[2]
    rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    rnd, failed = random.Random(1), 0
    for r in range(rounds):
        ok, np_, nl, mp, ml = one_round(binary, seed, rnd)
        bad = ok != 'ok' or mp != 0 or ml != 0 or not (np_ and nl)
        failed += bool(bad)
        print(f'round {r}: integrity={ok} acked posts={np_} likes={nl} missing posts={mp} likes={ml}'
              f'{"  FAIL" if bad else ""}', flush=True)
    print('PASS' if not failed else f'FAIL ({failed} of {rounds} rounds)')
    sys.exit(1 if failed else 0)

if __name__ == '__main__':
    main()
