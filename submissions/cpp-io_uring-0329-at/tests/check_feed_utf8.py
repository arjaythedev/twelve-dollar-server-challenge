#!/usr/bin/env python3
"""Isolated UTF-8 feed and bounded dense-like-cache regressions (B/full, repo R)."""
import json
import os
from pathlib import Path
import resource
import shutil
import sqlite3
import subprocess
import sys
from contextlib import closing

B, R = map(Path, sys.argv[1:3])
D = B / 'utf8-cases'
D.mkdir(exist_ok=True)

def fresh(name):
    p = D / (name + '.db')
    for suffix in ('', '-wal', '-shm'):
        Path(str(p) + suffix).unlink(missing_ok=True)
    shutil.copyfile(R / 'seed/feed.db', p)
    return p

def reference(db):
    with closing(sqlite3.connect(db)) as c:
        return [dict(zip(('id', 'body', 'created_at', 'author', 'like_count'), row))
                for row in c.execute('SELECT p.id,p.body,p.created_at,u.username,'
                    '(SELECT count(*) FROM likes l WHERE l.post_id=p.id) '
                    'FROM posts p JOIN users u ON p.user_id=u.id '
                    'ORDER BY p.created_at DESC,p.id DESC LIMIT 20')]

def memory_limit():
    # A sparse billion-ID span must fall back, not allocate ~4 GiB.
    resource.setrlimit(resource.RLIMIT_AS, (256 * 1024**2, 256 * 1024**2))

def run(db, mode='check', extra=None):
    env = os.environ.copy()
    for key in ('NO_TABLE', 'NO_AUTHORS'):
        env.pop(key, None)
    env.update(extra or {})
    result = subprocess.run([str(B / 'full'), str(db), mode], env=env,
        capture_output=True, text=True, encoding='utf-8', preexec_fn=memory_limit)
    assert result.returncode == 0, (db.name, extra, result.returncode, result.stderr)
    rows = {}
    for line in result.stdout.splitlines():
        key, _, value = line.partition(' ')
        rows.setdefault(key, []).append(value)
    key = 'after_feed' if mode == 'writes' else 'feed'
    assert json.loads(rows[key][-1])['posts'] == reference(db), (db.name, extra, key)
    if mode == 'writes':
        assert rows['duplicate'] == ['0']
        assert json.loads(rows['after_commit'][0])['post']['like_count'] == 2
    return rows

checks = 0
for label, char in (('ascii', 'a'), ('cjk', '汉'), ('emoji', '😀')):
    p = fresh(label)
    with closing(sqlite3.connect(p)) as c, c:
        # Keep the top-20 timestamps in descending rowid order for table fast path.
        ids = [row[0] for row in c.execute('SELECT id FROM posts ORDER BY id DESC LIMIT 20')]
        for rank, pid in enumerate(reversed(ids)):
            c.execute('UPDATE posts SET body=?,created_at=? WHERE id=?',
                (char * 500, f'2090-01-01T00:00:{rank:02d}.000Z', pid))
    expected = reference(p)
    assert len(expected) == 20 and all(len(row['body']) == 500 for row in expected)
    for extra in ({}, {'NO_TABLE': '1'}, {'NO_AUTHORS': '1'},
                  {'NO_TABLE': '1', 'NO_AUTHORS': '1'}):
        run(p, extra=extra)
        checks += 1

# Initial sparse histogram and insertion-induced growth exercise distinct guards.
for label, initial_high_like in (('sparse_initial', True), ('sparse_growth', False)):
    p = fresh(label)
    with closing(sqlite3.connect(p)) as c, c:
        c.execute('DELETE FROM likes')
        c.execute('INSERT INTO posts(id,user_id,body,created_at) VALUES(?,?,?,?)',
                  (1000000001, 1, 'sparse high ID', '2090-01-01T00:00:00.000Z'))
        c.execute('INSERT INTO likes(user_id,post_id) VALUES(1,1)')
        if initial_high_like:
            c.execute('INSERT INTO likes(user_id,post_id) VALUES(1,1000000001)')
    run(p, mode='writes')
    checks += 1
print(f'PASS: {checks} UTF-8/feed-fallback/sparse-like cases (256 MiB address-space limit)')
