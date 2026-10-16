import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import hmac
import http.client
import json
import socket
import sqlite3
import time
from urllib.parse import urlsplit

parser = argparse.ArgumentParser()
parser.add_argument('url', nargs='?', default='http://127.0.0.1:3000')
parser.add_argument('--db', help='local disposable server database for writer-lock failure test')
args = parser.parse_args()
target = urlsplit(args.url)
host, port = target.hostname, target.port or 80

def b64(data):
    return base64.urlsafe_b64encode(data).rstrip(b'=')

def token(payload=None, algorithm='HS256'):
    if payload is None:
        payload = {'sub': '1', 'username': 'claim "author" café', 'exp': time.time()+3600}
    message = b64(json.dumps({'alg': algorithm}).encode()) + b'.' + b64(json.dumps(payload).encode())
    return (message + b'.' + b64(hmac.new(b'twelve-dollar-challenge', message, hashlib.sha256).digest())).decode()

def request(method, path, data=None, jwt=None):
    c = http.client.HTTPConnection(host, port, timeout=10)
    headers = {'Authorization': 'Bearer '+jwt} if jwt is not None else {}
    if data is not None:
        data = json.dumps(data, ensure_ascii=False).encode()
        headers['Content-Type'] = 'application/json'
    c.request(method, path, body=data, headers=headers)
    r = c.getresponse()
    result = r.status, json.loads(r.read())
    c.close()
    return result

checks = 0
def check(condition, label):
    global checks
    assert condition, label
    checks += 1
    print('ok', label, flush=True)

valid = token()
status, post = request('POST', '/posts', {'body': '\ufeff\u2000  café ✓ "quoted" \\ \n \t\u3000'}, valid)
pid = post['post']['id']
check(status == 201 and post['post']['body'] == 'café ✓ "quoted" \\', 'Unicode trim/escape')
check(post['post']['author'] == 'claim "author" café', 'create uses JWT username')
status, readback = request('GET', f'/posts/{pid}')
check(status == 200 and readback['post']['author'] == 'golden_ember_1', 'read author comes from SQLite')
check(readback['post']['body'] == post['post']['body'], 'committed read-after-write')
for body, wanted in [('🧠'*500, 201), ('🧠'*501, 400)]:
    check(request('POST', '/posts', {'body': body}, valid)[0] == wanted, 'Unicode character limit '+str(wanted))
for payload in [
    {'sub': 1, 'username': 'x', 'exp': time.time()+10},
    {'sub': '0', 'username': 'x', 'exp': time.time()+10},
    {'sub': '-1', 'username': 'x', 'exp': time.time()+10},
    {'sub': '1', 'username': 12, 'exp': time.time()+10},
    {'sub': '1', 'username': 'x', 'exp': True},
    {'sub': '1', 'username': 'x', 'exp': time.time()-1},
    {'sub': '1', 'username': 'x', 'nbf': time.time()+100, 'exp': time.time()+200},
    [],
]:
    check(request('POST', '/posts', {'body': 'x'}, token(payload))[0] == 401, 'invalid claims '+repr(payload))
check(request('POST', '/posts', {'body': 'x'}, token(algorithm='HS512'))[0] == 401, 'reject alternate algorithm')
for data in [None, [], 1, 'x', {'body': None}]:
    check(request('POST', '/posts', data, valid)[0] == 400, 'invalid JSON body shape '+repr(data))

# A valid document below 16 KiB can need much more memory for its parsed nodes.
c = http.client.HTTPConnection(host,port,timeout=15)
dense = json.dumps({'body':'dense JSON','ignored':[0]*7800},separators=(',',':')).encode()
assert len(dense)<16384
c.request('POST','/posts',dense,{'Authorization':'Bearer '+valid,'Content-Type':'application/json'})
r = c.getresponse()
status, result = r.status,json.loads(r.read())
c.close()
check(status == 201 and result['post']['body']=='dense JSON', 'bounded arena accepts dense valid JSON')
dense_token = token({'sub':'1','username':'dense claims','exp':time.time()+3600,'ignored':[0]*1900})
assert len(dense_token)+7<8192
c = http.client.HTTPConnection(host,port,timeout=15)
c.request('POST','/posts',dense,{'Authorization':'Bearer '+dense_token,'Content-Type':'application/json'})
r = c.getresponse()
status, result = r.status,json.loads(r.read())
c.close()
check(status == 201 and result['post']['author']=='dense claims', 'bounded arena holds dense claims and body together')

# Exactly full bodies exercise yyjson's path without in-situ padding room.
full = b'{"body":"full body boundary"}'
full += b' '*(16384-len(full))
c = http.client.HTTPConnection(host,port,timeout=15)
c.request('POST','/posts',full,{'Authorization':'Bearer '+valid,'Content-Type':'application/json'})
r = c.getresponse()
status, result = r.status,json.loads(r.read())
c.close()
check(status == 201 and result['post']['body']=='full body boundary', 'full 16 KiB JSON body preserves parser bounds')

c = http.client.HTTPConnection(host,port,timeout=15)
for text, jwt in [('owned \\ " strings café',dense_token), ('fresh',valid)]:
    c.request('POST','/posts',json.dumps({'body':text}).encode(),{'Authorization':'Bearer '+jwt})
    r = c.getresponse()
    status, result = r.status,json.loads(r.read())
    check(status == 201 and result['post']['body']==text, 'keep-alive JSON storage survives and resets')
c.close()

with ThreadPoolExecutor(max_workers=16) as workers:
    responses = list(workers.map(lambda _: request('POST', f'/posts/{pid}/like', jwt=valid), range(32)))
check(sum(status == 201 for status, _ in responses) == 1, 'concurrent like inserts once')
check(sum(status == 200 for status, _ in responses) == 31, 'concurrent repeat likes')
check(request('GET', f'/posts/{pid}')[1]['post']['like_count'] == 1, 'fresh count after concurrent likes')
tampered = valid.rsplit('.', 1)[0] + '.' + ('A' if valid.rsplit('.',1)[1][0] != 'A' else 'B') + valid.rsplit('.',1)[1][1:]
check(request('POST', '/posts', {'body': 'x'}, tampered)[0] == 401, 'verify again after valid-token requests')

if args.db:
    lock = sqlite3.connect(args.db, isolation_level=None)
    before = lock.execute('SELECT count(*) FROM posts').fetchone()[0]
    lock.execute('BEGIN IMMEDIATE')
    check(request('POST', '/posts', {'body': 'must not acknowledge'}, valid)[0] == 500, 'writer lock never returns success')
    lock.execute('ROLLBACK')
    check(lock.execute('SELECT count(*) FROM posts').fetchone()[0] == before, 'failed write leaves no row')
    lock.close()
    check(request('POST', '/posts', {'body': 'writer recovered'}, valid)[0] == 201, 'statement recovers after SQLite busy')

def raw_responses(sock, expected):
    stream = sock.makefile('rb')
    result = []
    for _ in range(expected):
        line = stream.readline()
        assert line.startswith(b'HTTP/1.1 '), line
        status = int(line.split()[1])
        headers = {}
        while True:
            line = stream.readline()
            if line == b'\r\n': break
            key, value = line.split(b':', 1)
            headers[key.lower()] = value.strip()
        data = stream.read(int(headers[b'content-length']))
        result.append((status, json.loads(data)))
    stream.close()
    return result

s = socket.create_connection((host, port), timeout=10)
message = b'GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n'
for byte in message:
    s.sendall(bytes([byte]))
    time.sleep(.001)
check(raw_responses(s, 1)[0][0] == 200, 'fragmented request/header')
s.close()

s = socket.create_connection((host, port), timeout=10)
s.sendall(message*100)
check(all(status == 200 for status, _ in raw_responses(s, 100)), '100 pipelined requests preserve framing')
s.close()

s = socket.create_connection((host, port), timeout=10)
body = json.dumps({'body': 'chunked café ✓'}, ensure_ascii=False).encode()
head = ('POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer '+valid+'\r\nTransfer-Encoding: chunked\r\n\r\n').encode()
s.sendall(head)
for byte in body:
    s.sendall(b'1\r\n'+bytes([byte])+b'\r\n')
s.sendall(b'0\r\n\r\n')
result = raw_responses(s, 1)[0]
check(result[0] == 201 and result[1]['post']['body'] == 'chunked café ✓', 'chunked fragmented UTF-8 body')
s.close()

s = socket.create_connection((host, port), timeout=15)
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16384)
s.sendall(b'GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n'*60)
time.sleep(.2)
check(request('GET', '/health')[0] == 200, 'slow reader leaves other connections responsive')
check(all(status == 200 and len(data['posts']) == 20 for status, data in raw_responses(s, 60)), 'slow-reader pipelined output remains owned')
s.close()

s = socket.create_connection((host, port), timeout=10)
s.sendall(message)
s.shutdown(socket.SHUT_WR)
check(raw_responses(s, 1)[0][0] == 200, 'half-close still receives final response')
s.close()

s = socket.create_connection((host, port), timeout=10)
body = json.dumps({'body': 'x'*500}).encode()+b' '*8000
head = (f'POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {valid}\r\nContent-Length: {len(body)}\r\n\r\n').encode()
s.sendall(head+body)
s.shutdown(socket.SHUT_WR)
result = raw_responses(s, 1)[0]
check(result[0] == 201 and result[1]['post']['body'] == 'x'*500, 'half-close drains multiple input buffers')
s.close()

s = socket.create_connection((host, port), timeout=10)
s.sendall(message*300)
s.shutdown(socket.SHUT_WR)
check(all(status == 200 for status, _ in raw_responses(s, 300)), 'half-close drains pipelined requests')
s.close()

s = socket.create_connection((host, port), timeout=15)
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16384)
s.sendall(b'GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n'*60)
s.shutdown(socket.SHUT_WR)
time.sleep(.2)
check(all(status == 200 for status, _ in raw_responses(s, 60)), 'half-close preserves backpressured output')
s.close()

if args.db:
    # Force output well beyond 32 iovecs without changing the supplied schema.
    db = sqlite3.connect(args.db)
    username = db.execute('SELECT username FROM users WHERE id=1').fetchone()[0]
    ids = []
    author = 'u'*16000
    text = '\x01'*499+'x'
    try:
        db.execute('UPDATE users SET username=? WHERE id=1',(author,))
        for _ in range(20):
            ids.append(db.execute('INSERT INTO posts(user_id,body) VALUES(1,?)',(text,)).lastrowid)
        db.commit()
        status, feed = request('GET','/feed')
        check(status == 200 and len(feed['posts']) == 20 and
              all(p['body']==text and p['author']==author for p in feed['posts']),
              'large feed spans chunks and escapes control bytes')
        s = socket.create_connection((host,port),timeout=30)
        s.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,16384)
        s.sendall(b'GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n'*4)
        s.shutdown(socket.SHUT_WR)
        time.sleep(.1)
        check(request('GET','/health')[0] == 200, 'large chunked output preserves scheduler responsiveness')
        check(all(status == 200 and result == feed for status,result in raw_responses(s,4)),
              'large scatter/gather pipeline preserves content and framing')
        s.close()
    finally:
        db.execute('UPDATE users SET username=? WHERE id=1',(username,))
        db.executemany('DELETE FROM posts WHERE id=?',[(pid,) for pid in ids])
        db.commit()
        db.close()
print(f'passed={checks}', flush=True)
