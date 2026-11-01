"""Focused integration checks beyond the official golden tests (stdlib only)."""
import asyncio
import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import hmac
import http.client
import json
import os
import resource
from pathlib import Path
import shutil
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent

def check_listener_balance(database, environment):
    # Reduce only this test child's descriptor limit. 1280 held connections
    # exceed one ring's 224 direct slots while fitting safely across eight.
    def low_limit():
        resource.setrlimit(resource.RLIMIT_NOFILE, (256, 256))
    with (database.parent / 'listener-balance.log').open('w+') as log:
        server = subprocess.Popen([str(ROOT / 'bin/server')], env=environment,
                                  stdout=log, stderr=log, preexec_fn=low_limit)
        port = int(environment['PORT'])
        try:
            for _ in range(100):
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.1):
                        break
                except OSError:
                    time.sleep(.05)
            else:
                raise AssertionError('low-limit server did not start')
            async def held_connections():
                peers = await asyncio.gather(*(asyncio.open_connection('127.0.0.1', port) for _ in range(1280)))
                try:
                    async def check(reader, writer):
                        writer.write(b'GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n')
                        await writer.drain()
                        assert (await reader.readline()).startswith(b'HTTP/1.1 200 ')
                        headers = {}
                        while (line := await reader.readline()) != b'\r\n':
                            assert line, 'connection dropped at a ring boundary'
                            key, value = line.split(b':', 1)
                            headers[key.lower()] = value.strip()
                        body = json.loads(await reader.readexactly(int(headers[b'content-length'])))
                        assert body['status'] == 'ok' and body['db'] == 'ok'
                    for _ in range(2):
                        await asyncio.gather(*(check(reader, writer) for reader, writer in peers))
                finally:
                    for _, writer in peers:
                        writer.close()
                    await asyncio.gather(*(writer.wait_closed() for _, writer in peers), return_exceptions=True)
            asyncio.run(asyncio.wait_for(held_connections(), timeout=60))
        finally:
            server.terminate()
            try:
                server.wait(timeout=15)
            except subprocess.TimeoutExpired:
                server.kill(); server.wait()
            log.seek(0); text = log.read()
            if server.returncode != 0 or 'Sanitizer' in text or 'runtime error:' in text:
                raise AssertionError(f'low-limit server exit={server.returncode}\n{text}')
def token(payload, algorithm='HS256'):
    encode = lambda value: base64.urlsafe_b64encode(json.dumps(value, separators=(',', ':')).encode()).rstrip(b'=')
    data = encode({'alg': algorithm}) + b'.' + encode(payload)
    signature = base64.urlsafe_b64encode(hmac.new(b'twelve-dollar-challenge', data, hashlib.sha256).digest()).rstrip(b'=')
    return (data + b'.' + signature).decode()

def main():
    with tempfile.TemporaryDirectory() as directory:
        database = Path(directory) / 'feed.db'
        shutil.copyfile(ROOT.parent.parent / 'seed/feed.db', database)
        with socket.socket() as candidate:
            candidate.bind(('127.0.0.1', 0))
            port = candidate.getsockname()[1]
        environment = dict(os.environ, SQLITE_PATH=str(database), JWT_SECRET='twelve-dollar-challenge', HOST='127.0.0.1', PORT=str(port))
        log = (Path(directory) / 'server.log').open('w+')
        server = subprocess.Popen([str(ROOT / 'bin/server')], env=environment, stdout=log, stderr=log)
        passed = []
        def request(method, path, body=None, authorization=None):
            client = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
            headers = {} if authorization is None else {'Authorization': 'Bearer ' + authorization}
            if isinstance(body, str):
                body = body.encode('utf-8')
            client.request(method, path, body, headers)
            response = client.getresponse()
            result = response.status, json.loads(response.read())
            client.close()
            return result
        try:
            for _ in range(100):
                try:
                    if request('GET', '/health')[0] == 200:
                        break
                except OSError:
                    time.sleep(.05)
            else:
                raise AssertionError('server did not become healthy')
            payload = {'sub': '1', 'username': 'curvynes " ✓', 'exp': time.time() + 3600}
            valid = token(payload)
            for name, changed, expected in (
                ('missing exp', {k:v for k,v in payload.items() if k != 'exp'}, 'invalid or expired token'),
                ('string exp', dict(payload, exp='99999999999'), 'invalid or expired token'),
                ('null exp', dict(payload, exp=None), 'invalid or expired token'),
                ('expired exp', dict(payload, exp=1), 'invalid or expired token'),
                ('numeric sub', dict(payload, sub=1), 'invalid token payload'),
                ('zero sub', dict(payload, sub='0'), 'invalid token payload'),
                ('negative sub', dict(payload, sub='-1'), 'invalid token payload'),
                ('missing username', {k:v for k,v in payload.items() if k != 'username'}, 'invalid token payload'),
                ('non-string username', dict(payload, username={}), 'invalid token payload'),
            ):
                assert request('POST', '/posts', '{"body":"x"}', token(changed)) == (401, {'error': expected}), name
                passed.append(name)
            assert request('POST', '/posts', '{"body":"x"}', token(payload, 'HS512'))[0] == 401
            assert request('POST', '/posts', '{"body":"x"}', valid[:-3] + 'abc')[0] == 401
            passed.append('algorithm and signature enforcement')
            for body in ('"a"', '[]', 'null', 'true', '123'):
                assert request('POST', '/posts', body, valid) == (400, {'error':'body is required'})
            passed.append('non-object JSON validation')
            for text, expected in (('\u00a0\u2003 café 😀 \ufeff', 'café 😀'), ('😀' * 500, '😀' * 500), ('a\nb\tc"\\✓', 'a\nb\tc"\\✓')):
                status, post = request('POST', '/posts', json.dumps({'body': text}), valid)
                assert status == 201 and post['post']['body'] == expected
                assert post['post']['author'] == payload['username']
                assert request('GET', '/posts/' + str(post['post']['id']))[1]['post']['body'] == expected
            assert request('POST', '/posts', json.dumps({'body': '😀' * 501}), valid)[0] == 400
            assert request('POST', '/posts', '{"body":"\u2003\u00a0"}', valid)[0] == 400
            passed.append('Unicode trimming, 500/501 characters, escaping, and token username')
            status, created = request('POST', '/posts', '{"body":"concurrent like"}', valid)
            post_id = created['post']['id']
            with ThreadPoolExecutor(max_workers=16) as pool:
                results = list(pool.map(lambda _: request('POST', f'/posts/{post_id}/like', None, valid), range(64)))
            assert sum(status == 201 for status, _ in results) == 1
            assert all(status in (200, 201) for status, _ in results)
            assert request('GET', f'/posts/{post_id}')[1]['post']['like_count'] == 1
            passed.append('64 concurrent duplicate likes commit exactly one row')
            with sqlite3.connect(database) as connection:
                schema = connection.execute('SELECT type,name,sql FROM sqlite_schema ORDER BY name').fetchall()
                connection.execute("INSERT INTO posts(id,user_id,body,created_at) VALUES(900000,1,'future','2030-01-01T00:00:00.000Z')")
                connection.execute("INSERT INTO posts(id,user_id,body,created_at) VALUES(900002,1,'tie','2030-01-01T00:00:00.000Z')")
            posts = request('GET', '/feed')[1]['posts']
            assert [p['id'] for p in posts[:2]] == [900002, 900000]
            with sqlite3.connect(database) as connection:
                connection.execute('INSERT INTO likes(user_id,post_id) VALUES(2,900000)')
            assert request('GET', '/posts/900000')[1]['post']['like_count'] == 1
            assert request('GET', '/feed')[1]['posts'][1]['like_count'] == 1
            passed.append('live external commits, sparse ids, and timestamp ties')
            # HTTP pipelining, mixed header case, split input, and chunked bodies.
            with socket.create_connection(('127.0.0.1', port), timeout=10) as client:
                stream = client.makefile('rb')
                wire = b'GET /health HTTP/1.1\r\nHost: localhost\r\n\r\nGET /posts/1 HTTP/1.1\r\nHost: localhost\r\n\r\n'
                for byte in wire:
                    client.sendall(bytes([byte]))
                for _ in range(2):
                    assert stream.readline().startswith(b'HTTP/1.1 200 ')
                    headers = {}
                    while (line := stream.readline()) != b'\r\n':
                        key, value = line.split(b':', 1)
                        headers[key.lower()] = value.strip()
                    json.loads(stream.read(int(headers[b'content-length'])))
            passed.append('fragmented HTTP and pipelined response order')
            with socket.create_connection(('127.0.0.1', port), timeout=10) as client:
                body = b'{"body":"chunked"}'
                head = f'POST /posts HTTP/1.1\r\nHost: localhost\r\naUtHoRiZaTiOn: Bearer {valid}\r\nTransfer-Encoding: chunked\r\n\r\n'.encode()
                client.sendall(head + b'5\r\n' + body[:5] + b'\r\n' + f'{len(body)-5:x}\r\n'.encode() + body[5:] + b'\r\n0\r\n\r\n')
                response = http.client.HTTPResponse(client); response.begin()
                assert response.status == 201 and json.loads(response.read())['post']['body'] == 'chunked'
            passed.append('chunked request and case-insensitive headers')
            for framing in ('Content-Length: 1\r\nContent-Length: 2', 'Content-Length: 1\r\nTransfer-Encoding: chunked'):
                with socket.create_connection(('127.0.0.1', port), timeout=10) as client:
                    client.sendall(f'POST /posts HTTP/1.1\r\nHost: localhost\r\n{framing}\r\n\r\nx'.encode())
                    response = http.client.HTTPResponse(client); response.begin()
                    assert response.status == 400
                    response.read()
            passed.append('ambiguous HTTP framing rejected')
            with socket.create_connection(('127.0.0.1', port), timeout=10) as client:
                client.sendall(b'GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n')
                client.shutdown(socket.SHUT_WR)
                response = http.client.HTTPResponse(client); response.begin()
                assert response.status == 200 and len(json.loads(response.read())['posts']) == 20
            with socket.create_connection(('127.0.0.1', port), timeout=10) as client:
                stream = client.makefile('rb')
                client.sendall(b'GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\nGET /posts/1 HTTP/1.1\r\nHost: localhost\r\n\r\n')
                client.shutdown(socket.SHUT_WR)
                for key in ('posts', 'post'):
                    assert stream.readline().startswith(b'HTTP/1.1 200 ')
                    headers = {}
                    while (line := stream.readline()) != b'\r\n':
                        name, value = line.split(b':', 1)
                        headers[name.lower()] = value.strip()
                    assert key in json.loads(stream.read(int(headers[b'content-length'])))
                assert stream.read(1) == b''
            passed.append('write half-close preserves complete and pipelined responses')
            with socket.create_connection(('127.0.0.1', port), timeout=10) as client:
                body = b'{"body":"fragmented chunks"}'
                head = f'POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {valid}\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n'.encode()
                wire = head + f'{len(body):x}\r\n'.encode() + body + b'\r\n0\r\n\r\n'
                for offset in range(0, len(wire), 3):
                    client.sendall(wire[offset:offset+3])
                response = http.client.HTTPResponse(client); response.begin()
                assert response.status == 201 and json.loads(response.read())['post']['body'] == 'fragmented chunks'
                assert response.getheader('Connection') == 'close', dict(response.getheaders())
                assert client.recv(1) == b''
            passed.append('fragmented chunked body and graceful Connection close')
            def close_regression(_):
                with socket.create_connection(('127.0.0.1', port), timeout=5) as client:
                    body = b'{"body":"close regression"}'
                    head = f'POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {valid}\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n'.encode()
                    wire = head + f'{len(body):x}\r\n'.encode() + body + b'\r\n0\r\n\r\n'
                    for offset in range(0, len(wire), 3):
                        client.sendall(wire[offset:offset+3])
                    response = http.client.HTTPResponse(client); response.begin()
                    assert response.status == 201 and json.loads(response.read())['post']['body'] == 'close regression'
                    assert response.getheader('Connection') == 'close'
                    assert client.recv(1) == b''
            with ThreadPoolExecutor(max_workers=8) as pool:
                list(pool.map(close_regression, range(64)))
            passed.append('64 fragmented close requests finish with complete responses and EOF')
            def abandon(_):
                with socket.create_connection(('127.0.0.1', port), timeout=10) as client:
                    client.sendall(b'GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n')
            with ThreadPoolExecutor(max_workers=16) as pool:
                list(pool.map(abandon, range(512)))
            assert request('GET', '/health')[0] == 200
            passed.append('512 abandoned requests leave completion handling healthy')
            # Exercise the bounded range plan with scrambled timestamps, gaps in
            # the selected ids, extra rows in the range, and varying like counts.
            with sqlite3.connect(database) as connection:
                for i in range(64):
                    created_at = f'2031-01-01T00:{(i*7%64)//60:02d}:{i*7%64%60:02d}.000Z'
                    connection.execute('INSERT INTO posts(id,user_id,body,created_at) VALUES(?,1,?,?)', (1000000+i, f'range " ✓ {i}', created_at))
                    for user_id in range(2, 2+i%3):
                        connection.execute('INSERT INTO likes(user_id,post_id) VALUES(?,?)', (user_id, 1000000+i))
            with sqlite3.connect(database) as connection:
                rows = connection.execute('SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20').fetchall()
            expected = [dict(zip(('id','body','created_at','author','like_count'), row)) for row in rows]
            assert request('GET', '/feed') == (200, {'posts': expected})
            passed.append('bounded feed range matches SQL for reordered timestamps and varying counts')
            async def receive_burst():
                # Hold all sockets open before sending: more simultaneous
                # requests than one ring's receive pool, including abandoned
                # peers that must be removed from its pending retry queue.
                peers = await asyncio.gather(*(asyncio.open_connection('127.0.0.1', port) for _ in range(16384)))
                try:
                    # Accumulate input while this test's own child is paused,
                    # then release more requests than its shared buffer pool.
                    server.send_signal(signal.SIGSTOP)
                    try:
                        for _, writer in peers:
                            writer.write(b'GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n')
                        for _, writer in peers[:2048]:
                            writer.close()
                        await asyncio.gather(*(writer.drain() for _, writer in peers[2048:]))
                    finally:
                        server.send_signal(signal.SIGCONT)
                    async def consume(reader, writer):
                        await writer.drain()
                        assert (await reader.readline()).startswith(b'HTTP/1.1 200 ')
                        headers = {}
                        while (line := await reader.readline()) != b'\r\n':
                            assert line, 'response ended before its headers'
                            key, value = line.split(b':', 1)
                            headers[key.lower()] = value.strip()
                        body = await reader.readexactly(int(headers[b'content-length']))
                        assert json.loads(body) == {'posts': expected}
                    await asyncio.gather(*(consume(reader, writer) for reader, writer in peers[2048:]))
                finally:
                    for _, writer in peers:
                        writer.close()
                    await asyncio.gather(*(writer.wait_closed() for _, writer in peers), return_exceptions=True)
            asyncio.run(asyncio.wait_for(receive_burst(), timeout=60))
            assert request('GET', '/health')[0] == 200
            passed.append('16384-connection receive burst recovers with 2048 abandoned peers')
            with sqlite3.connect(database) as connection:
                assert connection.execute('SELECT type,name,sql FROM sqlite_schema ORDER BY name').fetchall() == schema
                assert connection.execute('PRAGMA integrity_check').fetchone() == ('ok',)
                assert not connection.execute('PRAGMA foreign_key_check').fetchmany(1)
            passed.append('schema unchanged and database integrity preserved')
        finally:
            server.terminate()
            try:
                server.wait(timeout=15)
            except subprocess.TimeoutExpired:
                server.kill(); server.wait()
            log.seek(0); text = log.read(); log.close()
            if sys.exc_info()[0] is not None:
                print(text, file=sys.stderr)
            if server.returncode != 0 or 'Sanitizer' in text or 'runtime error:' in text:
                raise AssertionError(f'server exit={server.returncode}\n{text}')
        # Ring destruction retires kernel resources asynchronously after close.
        # Keep the independent low-limit child from overlapping that retirement.
        time.sleep(1)
        check_listener_balance(database, environment)
        passed.append('1280 held connections survive ring boundaries at a 256-descriptor limit')
        for name in passed:
            print('PASS:', name)
        print(f'Extended checks passed: {len(passed)}')

if __name__ == '__main__':
    main()
