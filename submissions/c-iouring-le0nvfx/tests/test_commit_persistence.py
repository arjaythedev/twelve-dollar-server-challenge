#!/usr/bin/env python3
"""Check that acknowledged writes survive an abrupt server-process exit."""
import argparse
import base64
import concurrent.futures
import contextlib
import hashlib
import hmac
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import sqlite3
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument('--start', type=Path, required=True)
parser.add_argument('--seed', type=Path, required=True)
a = parser.parse_args()
start = a.start.resolve()
seed = a.seed.resolve()
with socket.socket() as sock:
    sock.bind(('127.0.0.1', 0))
    port = sock.getsockname()[1]
base = f'http://127.0.0.1:{port}'
enc = lambda b: base64.urlsafe_b64encode(b).rstrip(b'=')
now = int(time.time())
header = enc(b'{"alg":"HS256","typ":"JWT"}')
payload = enc(json.dumps({'sub':'1','username':'golden_ember_1',
                          'iat':now,'exp':now+3600}, separators=(',', ':')).encode())
message = header+b'.'+payload
secret = b'twelve-dollar-challenge'
token = (message+b'.'+enc(hmac.new(secret,message,hashlib.sha256).digest())).decode()

def request(path, body=None):
    data = None if body is None else json.dumps({'body': body}).encode()
    req = urllib.request.Request(base+path, data=data, headers={
        'Authorization':'Bearer '+token, 'Content-Type':'application/json'})
    with urllib.request.urlopen(req, timeout=15) as response:
        return response.status, json.load(response)

def launch(db, log):
    env=os.environ.copy()
    env.update(HOST='127.0.0.1', PORT=str(port), SQLITE_PATH=str(db),
               JWT_SECRET=secret.decode())
    proc=subprocess.Popen([str(start)], env=env, stdout=log, stderr=subprocess.STDOUT)
    deadline=time.monotonic()+60
    while time.monotonic()<deadline:
        if proc.poll() is not None:
            log.flush()
            raise RuntimeError('server exited during startup: ' + Path(log.name).read_text())
        try:
            if request('/health')[0] == 200: return proc
        except (OSError, urllib.error.URLError): time.sleep(.1)
    proc.kill(); proc.wait()
    raise RuntimeError('server did not start within 60 seconds')

with tempfile.TemporaryDirectory(prefix='12d-commit-') as tmp:
    db=Path(tmp)/'feed.db'
    shutil.copyfile(seed, db)
    with (Path(tmp)/'server.log').open('wb') as log:
        proc=launch(db, log)
        try:
            def create(i):
                body=f'commit-persistence-{os.getpid()}-{i}'
                status, result=request('/posts', body)
                if status != 201 or result['post']['body'] != body:
                    raise AssertionError(f'bad response: {status} {result}')
                return result['post']['id'],body
            with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
                acknowledged=list(pool.map(create, range(64)))
            os.kill(proc.pid, signal.SIGKILL)
            proc.wait(timeout=10)
            with contextlib.closing(sqlite3.connect(db)) as conn:
                if conn.execute('pragma integrity_check').fetchone()[0] != 'ok':
                    raise AssertionError('integrity check failed')
                if conn.execute('pragma foreign_key_check').fetchone() is not None:
                    raise AssertionError('foreign key check failed')
                for post_id,body in acknowledged:
                    row=conn.execute('select body from posts where id=?',(post_id,)).fetchone()
                    if row != (body,): raise AssertionError(f'acknowledged post {post_id} lost')
            proc=launch(db, log)
            for post_id,body in acknowledged:
                status,result=request(f'/posts/{post_id}')
                if status != 200 or result['post']['body'] != body:
                    raise AssertionError(f'post {post_id} missing after restart')
            print(f'PASS: {len(acknowledged)} acknowledged writes survived SIGKILL and restart')
        finally:
            if proc.poll() is None:
                proc.terminate()
                try: proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill(); proc.wait()
