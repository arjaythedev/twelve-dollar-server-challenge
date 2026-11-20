#!/usr/bin/env python3
"""foreign_writer.py LIVE_DB SERVER_LOG — another connection writes to the database (a post older
than the newest, which breaks Feed.Db.Inv's created_at order, and a like). The server must notice
(SQLite's data version), drop its native plans, and answer exactly what SQL says."""
import json, os, sqlite3, sys, time, http.client
db_path, log = sys.argv[1], sys.argv[2]
if 'sqlite: vfs unix-excl' in open(log, errors='replace').read():
    # Exclusive mode: the server owns the database file. Another process must be refused, for
    # reading and for writing, and the server must go on answering.
    HOST, PORT = os.environ.get('HOST', '127.0.0.1'), int(os.environ.get('PORT', '3000'))
    refused = []
    for what, sql in (('read', 'SELECT count(*) FROM posts'), ('write', "INSERT INTO likes (user_id, post_id) VALUES (7, 1)")):
        try:
            con = sqlite3.connect(db_path, timeout=0.5)
            con.execute(sql).fetchall()
            con.commit()
            con.close()
        except sqlite3.OperationalError as e:
            assert 'locked' in str(e), e
            refused.append(what)
    assert refused == ['read', 'write'], f'another process was not refused: {refused}'
    c = http.client.HTTPConnection(HOST, PORT, timeout=10); c.request('GET', '/feed')
    r = c.getresponse(); body = json.loads(r.read()); c.close()
    assert r.status == 200 and len(body['posts']) == 20
    print('foreign writer (exclusive mode): another process refused for read and write; server still answers')
    sys.exit(0)
HOST, PORT = os.environ.get('HOST', '127.0.0.1'), int(os.environ.get('PORT', '3000'))
fields = ['id', 'body', 'created_at', 'author', 'like_count']
q = '''SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id=p.id)
       FROM posts p JOIN users u ON u.id=p.user_id'''
def get(path):
    c = http.client.HTTPConnection(HOST, PORT, timeout=10); c.request('GET', path)
    r = c.getresponse(); b = r.read(); c.close(); return r.status, json.loads(b)
con = sqlite3.connect(db_path, timeout=10)
con.execute("INSERT INTO posts (user_id, body, created_at) VALUES (5, 'written by another connection', '2000-01-01T00:00:00.000Z')")
new_id = con.execute('SELECT max(id) FROM posts').fetchone()[0]
con.execute('INSERT OR IGNORE INTO likes (user_id, post_id) VALUES (7, ?)', (new_id,))
con.commit()
st, feed = get('/feed')
want = [dict(zip(fields, r)) for r in con.execute(q + ' ORDER BY p.created_at DESC, p.id DESC LIMIT 20')]
assert st == 200 and feed['posts'] == want, 'feed differs from SQL after a foreign write'
st, one = get(f'/posts/{new_id}')
assert st == 200 and one['post'] == dict(zip(fields, con.execute(q + ' WHERE p.id=?', (new_id,)).fetchone()))
con.close()
time.sleep(0.2)
text = open(log, errors='replace').read()
assert 'another connection wrote to the database' in text, 'foreign write not detected'
print(f'foreign writer: detected, native plans off, feed and post {new_id} match SQL')
