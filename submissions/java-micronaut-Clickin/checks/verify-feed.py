#!/usr/bin/env python3
"""Build first; this starts an isolated server against disposable feed fixtures."""
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import time
import urllib.request

submission = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temp, socket.socket() as port:
    port.bind(('127.0.0.1', 0))
    address = f'http://127.0.0.1:{port.getsockname()[1]}'
    env = dict(os.environ, HOST='127.0.0.1', PORT=str(port.getsockname()[1]),
               SQLITE_PATH=f'{temp}/feed.db')
    port.close()
    with sqlite3.connect(env['SQLITE_PATH']) as db:
        db.executescript((submission.parents[1] / 'schema.sql').read_text())
    with open(f'{temp}/server.log', 'w+') as log:
        server = subprocess.Popen(['bash', str(submission / 'start.sh')], env=env,
                                  stdout=log, stderr=subprocess.STDOUT)
        try:
            def get(path):
                with urllib.request.urlopen(address + path, timeout=2) as response:
                    return json.load(response)
            deadline = time.monotonic() + 60
            while True:
                try:
                    assert get('/health')['db'] == 'ok'
                    break
                except OSError:
                    if server.poll() is not None or time.monotonic() >= deadline:
                        log.seek(0)
                        raise AssertionError(log.read())
                    time.sleep(0.1)
            assert get('/feed') == {'posts': []}
            timestamp = '2025-01-01T00:00:00.000Z'
            body = 'quote " slash \\ newline\n nul\x00 한글 😀'
            with sqlite3.connect(env['SQLITE_PATH']) as db:
                db.execute("INSERT INTO users(id,username) VALUES(1,'author')")
                db.executemany('INSERT INTO posts(id,user_id,body,created_at) VALUES(?,1,?,?)',
                               [(i, body, timestamp) for i in range(1, 23)])
                db.execute('INSERT INTO likes(user_id,post_id) VALUES(1,22)')
            expected = [{'id': i, 'body': body, 'created_at': timestamp, 'author': 'author',
                         'like_count': int(i == 22)} for i in range(22, 2, -1)]
            assert get('/feed') == {'posts': expected}
            with sqlite3.connect(env['SQLITE_PATH']) as db:
                db.execute('DELETE FROM likes')
                db.execute('DELETE FROM posts WHERE id != 1')
                db.execute("UPDATE posts SET body='changed' WHERE id=1")
            expected = [dict(expected[-1], id=1, body='changed', like_count=0)]
            assert get('/feed') == {'posts': expected}
            print('PASS: empty/short feeds, timestamp ties, 20-row limit, JSON escaping and external updates')
        finally:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
