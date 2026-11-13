#!/usr/bin/env python3
"""Exercise real blocking ring handovers with the challenge's low fd limit.

Run after building and generating the seed. Uses a disposable database and
fails if startup silently selects epoll instead of the transport under test.
"""
import argparse
import json
import os
from pathlib import Path
import resource
import shutil
import socket
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--connections', type=int, default=600)
parser.add_argument('--nofile', type=int, default=512)
args = parser.parse_args()
# The load-generator process needs one ordinary fd per client. Only the
# server child receives the requested challenge limit below.
_, client_hard_limit = resource.getrlimit(resource.RLIMIT_NOFILE)
resource.setrlimit(resource.RLIMIT_NOFILE, (client_hard_limit, client_hard_limit))
assert args.connections + 32 < client_hard_limit, 'raise the client hard fd limit'
buffered = {}


def low_limit():
    resource.setrlimit(resource.RLIMIT_NOFILE, (args.nofile, args.nofile))


def request(sock, path='/health', body=None, token=None, send=True):
    method = 'POST' if body is not None else 'GET'
    headers = f'{method} {path} HTTP/1.1\r\nHost: localhost\r\n'
    if token:
        headers += f'Authorization: Bearer {token}\r\n'
    if body is not None:
        headers += f'Content-Type: application/json\r\nContent-Length: {len(body)}\r\n'
    if send:
        sock.sendall((headers + '\r\n').encode() + (body or b''))
    data = buffered.pop(sock, b'')
    while b'\r\n\r\n' not in data:
        part = sock.recv(4096)
        assert part, 'EOF before response headers'
        data += part
    head, data = data.split(b'\r\n\r\n', 1)
    length = next(int(line.split(b':', 1)[1]) for line in head.split(b'\r\n')[1:]
                  if line.lower().startswith(b'content-length:'))
    while len(data) < length:
        part = sock.recv(4096)
        assert part, 'EOF before response body'
        data += part
    buffered[sock] = data[length:]
    return int(head.split()[1]), data[:length]


def run(forced):
    count = 600 if forced else args.connections
    def connect(index):
        # Multiple loopback source IPs avoid mistaking the client's ephemeral
        # port ceiling for the server's fixed-file table ceiling.
        sock = socket.socket()
        sock.settimeout(3)
        sock.bind((f'127.0.0.{2 + index // 20000}', 0))
        sock.connect(('127.0.0.1', port))
        return sock
    with tempfile.TemporaryDirectory(prefix='native-capacity-') as work:
        db = Path(work) / 'feed.db'
        shutil.copyfile(ROOT / 'seed/feed.db', db)
        probe = socket.socket()
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
        probe.close()
        env = dict(os.environ, HOST='127.0.0.1', PORT=str(port), SQLITE_PATH=str(db),
                   JWT_SECRET='twelve-dollar-challenge', FEED_WORKERS='1')
        for key in ('FEED_EPOLL', 'FEED_TABLE', 'FEED_HANDOVER'):
            env.pop(key, None)
        if forced:
            env.update(FEED_TABLE='128', FEED_HANDOVER='16')
        sockets = []
        with open(Path(work) / 'server.log', 'w+') as log:
            server = subprocess.Popen([str(HERE / 'bin/feed')], env=env,
                                      preexec_fn=low_limit, stdout=log, stderr=log)
            try:
                for _ in range(300):
                    log.flush()
                    text = Path(log.name).read_text()
                    assert 'transport: epoll' not in text, text
                    if 'io_uring: 1 rings' in text:
                        break
                    assert server.poll() is None, text
                    time.sleep(.1)
                else:
                    raise AssertionError('io_uring did not start:\n' + text)
                # More than one production-sized table at RLIMIT_NOFILE=512,
                # or more than four forced 128-slot tables. Keep them ALL open.
                for i in range(count):
                    sock = connect(i)
                    sockets.append(sock)
                    try:
                        assert request(sock)[0] == 200, i
                    except Exception as error:
                        raise AssertionError(f'connection {i + 1}/{count} failed') from error
                token = json.loads((ROOT / 'seed/tokens.json').read_text())[0]['token']
                # Acknowledge writes on both sides of the handover, then read
                # their committed rows through another lane's SQLite snapshot.
                for sock in (sockets[0], sockets[-1]):
                    status, body = request(sock, '/posts', b'{"body":"capacity probe"}', token)
                    assert status == 201, (status, body)
                    post_id = json.loads(body)['post']['id']
                    assert request(sockets[300], f'/posts/{post_id}')[0] == 200
                for sock in sockets:
                    assert request(sock)[0] == 200
                # More than a bounded CQ pass, with no new traffic to wake a
                # sleeping issuer after its eventfd counter has been drained.
                burst = sockets[-600:]
                for sock in burst:
                    sock.sendall(b'GET /health HTTP/1.1\r\nHost: x\r\n\r\n' * 4)
                for sock in burst:
                    for _ in range(4):
                        assert request(sock, send=False)[0] == 200
                for sock in sockets[:100]:
                    sock.close()
                del sockets[:100]
                for _ in range(100):
                    sock = connect(count + _)
                    sockets.append(sock)
                    assert request(sock)[0] == 200
                text = Path(log.name).read_text()
                assert 'ring 1 opened' in text, text
                assert 'transport: epoll' not in text, text
                assert 'direct close failed' not in text, text
                assert server.poll() is None, text
                print(f'PASS: {count} held connections, cross-ring writes, slot reuse; '
                      f'forced={forced}, nofile={args.nofile}')
            finally:
                for sock in sockets:
                    sock.close()
                server.terminate()
                server.wait(timeout=10)
                buffered.clear()
                print(Path(log.name).read_text())


run(False)
run(True)
