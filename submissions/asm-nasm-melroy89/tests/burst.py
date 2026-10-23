#!/usr/bin/env python3
"""Queue a feed burst while the owner is stopped; validate every complete reply.
Exercises completion backlog beyond both provided-buffer and output-slab counts.
Native functional test, not an official capacity score.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import resource
import shutil
import signal
import socket
import sqlite3
import subprocess
import tempfile
import time
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--port',type=int,default=3197)
p.add_argument('--connections',type=int,default=5000)
p.add_argument('--server-arg',action='append',default=[])
a=p.parse_args()
soft,hard=resource.getrlimit(resource.RLIMIT_NOFILE)
resource.setrlimit(resource.RLIMIT_NOFILE,(max(soft,min(hard,a.connections+1024)),hard))
here=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(dir=here/'build',prefix='burst-') as tmp:
    db=Path(tmp)/'feed.db';shutil.copyfile(here.parents[1]/'seed/feed.db',db)
    with sqlite3.connect(db) as c:
        rows=c.execute('SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20').fetchall()
        expected={'posts':[dict(zip(['id','body','created_at','author','like_count'],row)) for row in rows]}
    c.close()
    env=os.environ|dict(SQLITE_PATH=str(db),JWT_SECRET='burst-test',HOST='127.0.0.1',PORT=str(a.port))
    sockets=[]
    with open(Path(tmp)/'server.log','w+') as log:
        proc=subprocess.Popen([str(here/'bin/server'),*a.server_arg],env=env,stdout=log,stderr=log)
        try:
            time.sleep(.2)
            assert proc.poll() is None
            for _ in range(a.connections):
                s=socket.create_connection(('127.0.0.1',a.port),timeout=10)
                sockets.append(s)
            time.sleep(.2)
            proc.send_signal(signal.SIGSTOP)
            for s in sockets:s.sendall(b'GET /feed HTTP/1.1\r\nHost: burst\r\n\r\n')
            started=time.monotonic()
            proc.send_signal(signal.SIGCONT)
            received={}
            passed=0
            with selectors.DefaultSelector() as sel:
                for s in sockets:
                    s.setblocking(False);sel.register(s,selectors.EVENT_READ);received[s]=bytearray()
                while sel.get_map():
                    assert proc.poll() is None,'server exited'
                    assert time.monotonic()-started<30,('burst did not drain',passed)
                    for key,_ in sel.select(.1):
                        s=key.fileobj
                        chunk=s.recv(65536)
                        assert chunk,('socket closed before response',passed)
                        received[s].extend(chunk)
                        data=received[s]
                        boundary=data.find(b'\r\n\r\n')
                        if boundary<0:continue
                        header=data[:boundary]
                        length=int(next(line.split(b':',1)[1] for line in header.split(b'\r\n') if line.lower().startswith(b'content-length:')))
                        assert length<65536
                        if len(data)<boundary+4+length:continue
                        assert header.startswith(b'HTTP/1.1 200'),bytes(header)
                        assert json.loads(data[boundary+4:boundary+4+length])==expected
                        sel.unregister(s);del received[s];passed+=1
            print(json.dumps(dict(connections=passed,elapsed_seconds=time.monotonic()-started,args=a.server_arg,binary_sha256=hashlib.sha256((here/'bin/server').read_bytes()).hexdigest(),kind='native burst semantics, not capacity')),flush=True)
        finally:
            if proc.poll() is None:proc.send_signal(signal.SIGCONT)
            for s in sockets:s.close()
            proc.terminate()
            try:proc.wait(10)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
            log.seek(0);print(log.read(),end='')
