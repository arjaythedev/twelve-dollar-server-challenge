#!/usr/bin/env python3
"""Check SIGTERM while requests are waiting for bodies and admission is full."""
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import time

from check import DIR, ROOT, SECRET, token


def main():
    with socket.socket() as available:
        available.bind(('127.0.0.1', 0))
        port = available.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix='jdkhttp-admission-') as directory:
        db = Path(directory) / 'feed.db'
        shutil.copyfile(ROOT / 'seed/feed.db', db)
        env = dict(os.environ, SQLITE_PATH=str(db), JWT_SECRET=SECRET, HOST='127.0.0.1', PORT=str(port))
        connections = []
        with (Path(directory) / 'server.log').open('w+') as log:
            server = subprocess.Popen(['bash', str(DIR / 'start.sh')], env=env, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 30
                while True:
                    try:
                        with socket.create_connection(('127.0.0.1', port), timeout=1):
                            break
                    except OSError:
                        if server.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError('server did not start')
                        time.sleep(.1)
                header = (f'POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {token()}\r\n'
                          'Content-Length: 100\r\n\r\n').encode()
                for _ in range(1100):
                    conn = socket.create_connection(('127.0.0.1', port), timeout=10)
                    conn.sendall(header)
                    connections.append(conn)
                # A further health request cannot complete once all 1,024 slots
                # are occupied and additional requests are queued.
                probe = socket.create_connection(('127.0.0.1', port), timeout=10)
                connections.append(probe)
                probe.sendall(b'GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n')
                probe.settimeout(5)
                try:
                    response = probe.recv(4096)
                except socket.timeout:
                    response = None
                if response is not None:
                    raise AssertionError(f'admission did not saturate: {response!r}')
                started = time.monotonic()
                server.send_signal(signal.SIGTERM)
                server.wait(timeout=15)
                print(f'PASS: saturated admission and active body reads shut down in {time.monotonic() - started:.2f}s')
            finally:
                if server.poll() is None:
                    server.kill()
                    server.wait()
                for conn in connections:
                    conn.close()


if __name__ == '__main__':
    main()
