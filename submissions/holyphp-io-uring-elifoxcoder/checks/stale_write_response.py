"""Regression: commit an abandoned write, but never reply on a reused socket FD."""
import glob
import base64
import argparse
import http.client
import json
import os
import pathlib
import socket
import sqlite3
import sys
import time
import uuid

args=argparse.ArgumentParser(description=__doc__)
args.add_argument('--pid',type=int,required=True)
args.add_argument('--database',required=True)
args.add_argument('--tokens',required=True)
args.add_argument('--port',type=int,default=3000)
args.add_argument('--kind',choices=('post','like'),default='post')
opts=args.parse_args()
pid,kind=opts.pid,opts.kind
token = json.loads(pathlib.Path(opts.tokens).read_text())[0]['token']
marker = 'generation-probe-' + uuid.uuid4().hex
db = sqlite3.connect(opts.database, timeout=5)
db.execute('BEGIN IMMEDIATE')
uid=int(json.loads(base64.urlsafe_b64decode(token.split('.')[1]+'==='))['sub'])
post_id=None
if kind=='like':
    post_id=db.execute('SELECT id FROM posts WHERE NOT EXISTS (SELECT 1 FROM likes WHERE post_id=posts.id AND user_id=?) ORDER BY id DESC LIMIT 1',(uid,)).fetchone()[0]

def accepted_fd(client):
    port = client.getsockname()[1]
    inode = None
    for row in pathlib.Path('/proc/net/tcp').read_text().splitlines()[1:]:
        fields = row.split()
        if int(fields[1].split(':')[1],16)==opts.port and int(fields[2].split(':')[1],16)==port:
            inode=fields[9]; break
    if inode:
        for f in glob.glob(f'/proc/{pid}/fd/*'):
            try:
                if os.readlink(f)==f'socket:[{inode}]': return int(f.rsplit('/',1)[1])
            except FileNotFoundError: pass
    return None

old = socket.create_connection(('127.0.0.1',opts.port),timeout=3)
body = json.dumps({'body':marker} if kind=='post' else {}).encode()
route='/posts' if kind=='post' else f'/posts/{post_id}/like'
old.sendall((f'POST {route} HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {token}\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\n\r\n').encode()+body)
old_fd=None
for _ in range(40):
    old_fd=accepted_fd(old)
    if old_fd is not None: break
    time.sleep(.05)
assert old_fd is not None, 'fixture must use classic descriptors; run as root for proc access'
for _ in range(40):
    http = http.client.HTTPConnection('127.0.0.1',opts.port,timeout=3)
    http.request('GET','/stats'); stats=json.loads(http.getresponse().read()); http.close()
    if stats['server']['pending_writes'] > 0: break
    time.sleep(.05)
old.settimeout(.2)
if stats['server']['pending_writes'] == 0:
    raise AssertionError(('write not queued',old.recv(8192),stats))
old.shutdown(socket.SHUT_RDWR); old.close(); time.sleep(.15)
fresh=socket.create_connection(('127.0.0.1',opts.port),timeout=3)
time.sleep(.05)
new_fd=accepted_fd(fresh)
assert old_fd==new_fd, (old_fd,new_fd)
db.rollback()
fresh.settimeout(.5)
unsolicited=b''
try: unsolicited=fresh.recv(8192)
except socket.timeout: pass
assert not unsolicited, f'old write replied on new FD: {unsolicited!r}'
fresh.sendall(b'GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n')
fresh.settimeout(3); response=b''
while b'\r\n\r\n' not in response: response+=fresh.recv(8192)
header,data=response.split(b'\r\n\r\n',1)
length=int(next(x.split(b':',1)[1] for x in header.split(b'\r\n') if x.lower().startswith(b'content-length:')))
while len(data)<length: data+=fresh.recv(8192)
assert json.loads(data[:length])['db']=='ok'
fresh.close()
if kind=='post':
    persisted=db.execute('SELECT count(*) FROM posts WHERE body=?',(marker,)).fetchone()[0]
else:
    persisted=db.execute('SELECT count(*) FROM likes WHERE user_id=? AND post_id=?',(uid,post_id)).fetchone()[0]
assert persisted==1, persisted
print(json.dumps({'kind':kind,'fd_reused':True,'unsolicited_reply':False,'abandoned_write_persisted':persisted,'health':'ok'}))
