#!/usr/bin/env python3
"""Compare native reads with independent SQL on sparse, unordered, overflow rows."""
import base64
import hashlib
import hmac
import http.client
import json
import os
from pathlib import Path
import random
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

server = str(Path(sys.argv[1]).resolve())
schema = Path(sys.argv[2]).read_text()
secret = 'native-oracle-secret'
select = '''SELECT p.id,p.body,p.created_at,u.username,
    (SELECT count(*) FROM likes l WHERE l.post_id=p.id)
    FROM posts p JOIN users u ON u.id=p.user_id'''

def posts(db, suffix):
    return [dict(zip(('id', 'body', 'created_at', 'author', 'like_count'), r))
            for r in db.execute(select + suffix)]

with tempfile.TemporaryDirectory(prefix='native-oracle-') as temp:
    path = str(Path(temp, 'feed.db'))
    db = sqlite3.connect(path)
    db.executescript(schema)
    for i in range(1, 11):
        # The long name forces SQLite overflow pages in the author cursor.
        name = ('名\\"\n' * 2000) if i == 1 else f'user-{i}'
        db.execute('INSERT INTO users(id,username) VALUES(?,?)', (i, name))
    rng = random.Random(42819)
    ids = [1, 127, 128, 32767, 32768, 8388607, 8388608] + rng.sample(range(10000000, 90000000), 60)
    for i, pid in enumerate(ids):
        body = ('😀\\"\n' * 80) if i % 3 == 0 else 'short body'
        db.execute('INSERT INTO posts VALUES(?,?,?,?)',
                   (pid, i % 10 + 1, body, f'2026-01-{rng.randrange(1, 8):02d}T00:00:00.000Z'))
        for uid in range(1, rng.randrange(1, 11)):
            db.execute('INSERT INTO likes(user_id,post_id) VALUES(?,?)', (uid, pid))
    db.commit()
    expected_feed = posts(db, ' ORDER BY p.created_at DESC,p.id DESC LIMIT 20')
    expected_posts = {pid: posts(db, f' WHERE p.id={pid}')[0] for pid in ids}
    db.close()
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    env = dict(os.environ, SQLITE_PATH=path, JWT_SECRET=secret, HOST='127.0.0.1', PORT=str(port))
    log = open(Path(temp, 'server.log'), 'w+')
    proc = subprocess.Popen([server], env=env, stdout=log, stderr=log)
    conn = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
    def request(method, path, body=None, headers=None):
        conn.request(method, path, body=body, headers=headers or {})
        r = conn.getresponse()
        return r.status, json.loads(r.read())
    try:
        for _ in range(100):
            try:
                assert request('GET', '/health')[0] == 200
                break
            except (ConnectionError, OSError):
                conn.close()
                if proc.poll() is not None:
                    log.seek(0)
                    raise AssertionError(log.read())
                time.sleep(.05)
        else:
            raise AssertionError('server did not become healthy')
        assert request('GET', '/feed') == (200, {'posts': expected_feed})
        for pid in ids:
            assert request('GET', f'/posts/{pid}') == (200, {'post': expected_posts[pid]})
        assert request('GET', '/posts/999999999') == (404, {'error': 'post not found'})
        def b64(b):
            return base64.urlsafe_b64encode(b).rstrip(b'=')
        message = b64(b'{"alg":"HS256"}') + b'.' + b64(json.dumps(
            dict(sub='2', username='user-2', exp=int(time.time()) + 3600)).encode())
        token = message + b'.' + b64(hmac.digest(secret.encode(), message, hashlib.sha256))
        auth = {'Authorization': 'Bearer ' + token.decode(), 'Content-Type': 'application/json'}
        status, created = request('POST', '/posts', json.dumps({'body': 'fresh 😀 post'}), auth)
        assert status == 201
        pid = created['post']['id']
        assert request('GET', f'/posts/{pid}') == (200, created)
        assert request('POST', f'/posts/{pid}/like', None, auth)[0] == 201
        assert request('POST', f'/posts/{pid}/like', None, auth)[0] == 200
        assert request('GET', f'/posts/{pid}')[1]['post']['like_count'] == 1
        assert request('GET', '/feed')[1]['posts'][0]['id'] == pid
    finally:
        conn.close()
        proc.terminate()
        proc.wait(timeout=10)
        log.close()
    with sqlite3.connect(path) as db:
        assert db.execute('PRAGMA integrity_check').fetchone()[0] == 'ok'
        assert db.execute('SELECT body FROM posts WHERE id=?', (pid,)).fetchone()[0] == 'fresh 😀 post'
        assert db.execute('SELECT count(*) FROM likes WHERE post_id=?', (pid,)).fetchone()[0] == 1
print('SQL oracle passed: sparse IDs, unordered/tied timestamps, overflow UTF-8, live writes, WAL recovery')
