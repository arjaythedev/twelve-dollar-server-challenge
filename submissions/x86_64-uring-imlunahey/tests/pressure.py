#!/usr/bin/env python3
"""Admitted fragmented uploads must finish when the aggregate input budget is full."""
import argparse
import base64
import hashlib
import hmac
import json
import os
import pathlib
import shutil
import socket
import subprocess
import tempfile
import time
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--server-arg", action="append", default=[])
args = parser.parse_args()
ROOT = pathlib.Path(__file__).resolve().parents[1]
secret = b"pressure-test"
encode = lambda x: base64.urlsafe_b64encode(json.dumps(x).encode()).rstrip(b"=")
data = encode({"alg":"HS256"}) + b"." + encode({"sub":"1","username":"pressure","exp":int(time.time())+300})
auth = (data+b"."+base64.urlsafe_b64encode(hmac.digest(secret,data,"sha256")).rstrip(b"=")).decode()
body = json.dumps({"body":"pressure 😀 completion"},ensure_ascii=False).encode()
split = body.index("😀".encode())+2
head = (f"POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer {auth}\r\n"
        f"Content-Length: {len(body)}\r\nContent-Type: application/json\r\n\r\n").encode()
with tempfile.TemporaryDirectory() as tmp:
    db=pathlib.Path(tmp)/"feed.db";shutil.copyfile(ROOT.parents[1]/"seed/feed.db",db)
    env=os.environ|dict(SQLITE_PATH=str(db),JWT_SECRET=secret.decode(),HOST="127.0.0.1",PORT="3193")
    proc=subprocess.Popen([str(ROOT/"bin/server"),"--recv=multi","--transaction=batch",*args.server_arg],env=env)
    sockets=[]
    try:
        time.sleep(.2)
        for _ in range(920):
            s=socket.create_connection(("127.0.0.1",3193),timeout=5)
            sockets.append(s)
            s.sendall(head+body[:split])
        time.sleep(.2)
        # The first admitted uploads have their full bounded input AND response storage reserved.
        for s in sockets[:32]:
            s.sendall(body[split:])
            response=b""
            while b"\r\n\r\n" not in response:
                part=s.recv(65536);assert part,"admitted upload was closed";response+=part
            h,b=response.split(b"\r\n\r\n",1)
            length=int(next(x.split(b":",1)[1] for x in h.split(b"\r\n") if x.lower().startswith(b"content-length:")))
            while len(b)<length:
                part=s.recv(65536);assert part;b+=part
            assert h.startswith(b"HTTP/1.1 201")
            assert json.loads(b)["post"]["body"]=="pressure 😀 completion"
        # Fresh admission works after completed uploads release their reserved buffers.
        with socket.create_connection(("127.0.0.1",3193),timeout=5) as s:
            s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
            assert s.recv(4096).startswith(b"HTTP/1.1 200")
        print("PASS: input-pressure completion, split UTF-8, reserved replies and admission recovery")
    finally:
        for s in sockets:s.close()
        proc.terminate()
        try:proc.wait(10)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
