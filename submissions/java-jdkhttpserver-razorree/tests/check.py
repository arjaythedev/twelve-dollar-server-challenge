#!/usr/bin/env python3
"""Additional HTTP, durability, keep-alive and optional one-core smoke checks.

Requires a built submission, the seed database, and Python 3 standard library only.
Creates its own database copies and starts/stops its own Java processes.
"""
import argparse
import base64
import concurrent.futures
import datetime
import hmac
import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import tempfile
import time

DIR = Path(__file__).resolve().parents[1]
ROOT = DIR.parents[1]
SECRET = 'twelve-dollar-challenge'


def token(payload=None, algorithm='HS256'):
    if payload is None:
        payload = {'sub': '1', 'username': 'golden_ember_1', 'exp': int(time.time()) + 3600}
    def b64(value):
        return base64.urlsafe_b64encode(value).rstrip(b'=')
    head = b64(json.dumps({'alg': algorithm}).encode())
    body = b64(json.dumps(payload).encode())
    signed = head + b'.' + body
    return (signed + b'.' + b64(hmac.digest(SECRET.encode(), signed, 'sha256'))).decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpu', type=int, default=0)
    parser.add_argument('--k6', help='optional k6 executable to run a 1,000-VU, 30-second smoke hold')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    output = (args.output or ROOT / 'results' / datetime.datetime.now(datetime.timezone.utc).strftime('java-jdkhttpserver-%Y%m%dT%H%M%S%fZ')).resolve()
    output.mkdir(parents=True, exist_ok=False)
    subprocess.run(['bash', str(DIR / 'tests/check-json.sh'), str(output / 'json')], check=True, timeout=120)
    with socket.socket() as available:
        available.bind(('127.0.0.1', 0))
        port = available.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix='java-jdkhttpserver-') as directory:
        db = Path(directory) / 'feed.db'
        shutil.copyfile(ROOT / 'seed/feed.db', db)
        env = dict(os.environ, SQLITE_PATH=str(db), JWT_SECRET=SECRET, HOST='127.0.0.1', PORT=str(port))
        server = None
        log = (output / 'server.log').open('w')
        passed = ['Exact JSON bytes against Jackson oracle', 'Concurrent HMAC verification and rejection checks', 'Database commit/savepoint/failure/shutdown contracts']
        report = {'server_cpu': args.cpu, 'tests_passed': passed, 'status': 'running'}

        def request(method, path, body=None, auth=None, raw=False):
            headers = {}
            if body is not None:
                headers['Content-Type'] = 'application/json'
                if not isinstance(body, bytes):
                    body = json.dumps(body, ensure_ascii=False).encode()
            if auth:
                headers['Authorization'] = 'Bearer ' + auth
            conn = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
            try:
                conn.request(method, path, body, headers)
                response = conn.getresponse()
                data = response.read()
                return response.status, data if raw else json.loads(data)
            finally:
                conn.close()

        def stop(kill=False):
            nonlocal server
            if server is not None:
                server.kill() if kill else server.terminate()
                try:
                    server.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
                server = None

        def start():
            nonlocal server
            server = subprocess.Popen(['taskset', '-c', str(args.cpu), 'bash', str(DIR / 'start.sh')], env=env, stdout=log, stderr=log)
            for _ in range(120):
                if server.poll() is not None:
                    raise RuntimeError('Server exited during startup; see server.log')
                try:
                    if request('GET', '/health')[0] == 200:
                        report['server_affinity'] = sorted(os.sched_getaffinity(server.pid))
                        return
                except OSError:
                    pass
                time.sleep(.25)
            raise RuntimeError('Server startup timed out')

        def ok(name):
            passed.append(name)
            print('PASS ' + name, flush=True)

        try:
            start()
            valid = token()
            for count, status in [(500, 201), (501, 400)]:
                assert request('POST', '/posts', {'body': '\ufeff\u00a0' + '😀' * count + '\u3000'}, valid)[0] == status
            status, created = request('POST', '/posts', {'body': ' nul\u0000text "\\ café '}, valid)
            assert status == 201 and created['post']['body'] == 'nul\u0000text "\\ café'
            expected = {'post': {k: created['post'][k] for k in ('id', 'body', 'created_at', 'author', 'like_count')}}
            expected_bytes = json.dumps(expected, ensure_ascii=False, separators=(',', ':')).encode()
            assert request('GET', f"/posts/{created['post']['id']}", raw=True) == (200, expected_bytes)
            ok('Exact post response bytes, field order and escaping')
            for malformed in [b'{"body":"x"} {}', b'{"body":"\xff"}', b'{"body":"\\ud800"}', b'{', b'']:
                assert request('POST', '/posts', malformed, valid) == (400, {'error': 'malformed JSON body'})
            ok('JSON, Unicode code-point limits, reference whitespace and embedded NUL')

            now = int(time.time())
            for changes, message in [({'exp': now - 1}, 'invalid or expired token'),
                                     ({'exp': 'tomorrow'}, 'invalid or expired token'),
                                     ({'nbf': now + 300}, 'invalid or expired token'),
                                     ({'sub': 1}, 'invalid token payload'),
                                     ({'sub': '0'}, 'invalid token payload'),
                                     ({'username': None}, 'invalid token payload')]:
                payload = {'sub': '1', 'username': 'golden_ember_1', 'exp': now + 300} | changes
                assert request('POST', '/posts', {'body': 'auth'}, token(payload)) == (401, {'error': message})
            assert request('POST', '/posts', {'body': 'auth'}, token(algorithm='none'))[0] == 401
            broken = valid.split('.')
            broken[2] = ('A' if broken[2][0] != 'A' else 'B') + broken[2][1:]
            assert request('POST', '/posts', {'body': 'auth'}, '.'.join(broken))[0] == 401
            expiring = token({'sub': '1', 'username': 'golden_ember_1', 'exp': int(time.time()) + 2})
            assert request('POST', '/posts', {'body': 'before expiry'}, expiring)[0] == 201
            time.sleep(2.1)
            assert request('POST', '/posts', {'body': 'after expiry'}, expiring)[0] == 401
            assert request('POST', '/posts/abc/like') == (401, {'error': 'missing bearer token'})
            ok('JWT signature, algorithm, claim types, expiry on reuse and auth precedence')

            status, created = request('POST', '/posts', {'body': 'concurrent likes'}, valid)
            assert status == 201
            post_id = created['post']['id']
            with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
                statuses = list(pool.map(lambda _: request('POST', f'/posts/{post_id}/like', auth=valid)[0], range(16)))
            assert statuses.count(201) == 1 and statuses.count(200) == 15, statuses
            assert request('GET', f'/posts/{post_id}')[1]['post']['like_count'] == 1
            assert request('GET', '/feed')[1]['posts'][0]['id'] == post_id
            ok('16 concurrent duplicate likes insert exactly one row and update reads')

            nonexistent = token({'sub': '999999', 'username': 'missing', 'exp': now + 3600})
            assert request('POST', '/posts', {'body': 'must roll back'}, nonexistent)[0] == 500
            status, committed = request('POST', '/posts', {'body': 'durable after failed write'}, valid)
            assert status == 201
            durable_id = committed['post']['id']
            assert request('POST', f'/posts/{durable_id}/like', auth=valid)[0] == 201
            stop(kill=True)
            with sqlite3.connect(db) as connection:
                assert connection.execute('PRAGMA integrity_check').fetchone() == ('ok',)
                assert connection.execute('SELECT body FROM posts WHERE id=?', (durable_id,)).fetchone() == ('durable after failed write',)
                assert connection.execute('SELECT count(*) FROM likes WHERE post_id=?', (durable_id,)).fetchone() == (1,)
                assert connection.execute("SELECT count(*) FROM posts WHERE body='must roll back'").fetchone() == (0,)
                assert connection.execute('PRAGMA journal_mode').fetchone() == ('wal',)
            start()
            assert request('GET', f'/posts/{durable_id}')[1]['post']['like_count'] == 1
            ok('failed transaction isolation and acknowledged writes survive SIGKILL/restart')

            # One pipeline exercises empty, fixed-length, ignored, and chunked bodies.
            # A bodyless-request optimization must never consume the next request or JSON byte.
            fixed = b'{"body":"fixed transport body"}'
            chunked = b'{"body":"chunked transport body"}'
            auth_header = f'Authorization: Bearer {valid}\r\n'.encode()
            wire = (b'GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n'
                    b'GET /health HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n'
                    b'GET /health HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2048\r\n\r\n'
                    + b'x' * 2048
                    + b'POST /posts HTTP/1.1\r\nHost: localhost\r\n' + auth_header
                    + f'Content-Length: {len(fixed)}\r\n\r\n'.encode() + fixed
                    + b'POST /posts HTTP/1.1\r\nHost: localhost\r\n' + auth_header
                    + b'Transfer-Encoding: chunked\r\n\r\n'
                    + f'{len(chunked):x}\r\n'.encode() + chunked + b'\r\n0\r\n\r\n'
                    + b'GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
            with socket.create_connection(('127.0.0.1', port), timeout=10) as connection:
                connection.sendall(wire)
                with connection.makefile('rb') as stream:
                    for index, expected_status in enumerate((200, 200, 200, 201, 201, 200)):
                        status = stream.readline().split()
                        assert len(status) >= 2 and int(status[1]) == expected_status, status
                        headers = {}
                        while True:
                            line = stream.readline()
                            if line == b'\r\n':
                                break
                            assert line, 'Connection closed inside response headers'
                            key, value = line.split(b':', 1)
                            headers[key.lower()] = value.strip()
                        payload = json.loads(stream.read(int(headers[b'content-length'])))
                        if index in (3, 4):
                            assert payload['post']['body'] == ('fixed transport body' if index == 3 else 'chunked transport body')
            ok('pipelined empty, fixed-length, ignored and chunked request bodies preserve framing')

            connections = []
            try:
                for _ in range(256):
                    conn = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
                    connections.append(conn)
                    conn.request('GET', '/health')
                    response = conn.getresponse()
                    assert response.status == 200
                    response.read()
                original_socket = connections[0].sock
                print('Waiting 66 seconds to verify same-socket reuse with 256 idle connections...', flush=True)
                time.sleep(66)
                connections[0].request('GET', '/health')
                response = connections[0].getresponse()
                assert response.status == 200
                response.read()
                assert connections[0].sock is original_socket, 'Client reconnected instead of reusing socket'
            finally:
                for conn in connections:
                    conn.close()
            ok('same socket reused after 66 seconds, with more than 200 idle connections')

            if args.k6:
                stop()
                for suffix in ('-wal', '-shm'):
                    db.with_name(db.name + suffix).unlink(missing_ok=True)
                shutil.copyfile(ROOT / 'seed/feed.db', db)
                start()
                # Exclude both the server CPU and all its SMT siblings from the generator.
                siblings_file = Path(f'/sys/devices/system/cpu/cpu{args.cpu}/topology/thread_siblings_list')
                excluded = set()
                for group in siblings_file.read_text().strip().split(','):
                    bounds = [int(x) for x in group.split('-')]
                    excluded.update(range(bounds[0], bounds[-1] + 1))
                load_cpus = sorted(os.sched_getaffinity(0) - excluded)
                if not load_cpus:
                    raise RuntimeError('No separate CPU available for k6')
                report['load_generator_cpus'] = load_cpus
                command = ['taskset', '-c', ','.join(map(str, load_cpus)), args.k6, 'run', '--quiet',
                           '--summary-export', str(output / 'smoke.json'), '-e', f'BASE_URL=http://127.0.0.1:{port}',
                           '-e', 'VUS=1000', '-e', 'DURATION=30s', '-e', 'RAMP_UP=5s', str(ROOT / 'bench/load.js')]
                print('Running 1,000-VU smoke test on a fresh database; server restricted to one CPU...', flush=True)
                with (output / 'smoke.log').open('w') as smoke_log:
                    child = subprocess.Popen(command, stdout=smoke_log, stderr=subprocess.STDOUT)
                    peak_rss = 0
                    try:
                        while child.poll() is None:
                            status = Path(f'/proc/{server.pid}/status').read_text()
                            peak_rss = max(peak_rss, int(next(x.split()[1] for x in status.splitlines() if x.startswith('VmRSS:'))))
                            time.sleep(1)
                    finally:
                        if child.poll() is None:
                            child.terminate()
                            child.wait()
                report['smoke'] = {'exit_code': child.returncode, 'sampled_peak_rss_kib': peak_rss, 'command': command}
                assert child.returncode == 0, 'k6 thresholds failed; see smoke.log'
                ok('one-core k6 smoke test (not a capacity score)')
            report['status'] = 'passed'
        except BaseException as error:
            report.update(status='failed', error=str(error) or type(error).__name__)
            raise
        finally:
            stop()
            log.close()
            (output / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
            print(f'Results: {output}', flush=True)


if __name__ == '__main__':
    main()
