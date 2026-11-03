#!/usr/bin/env python3
"""Functional multi-ring live-socket proof; not a user-capacity score."""
import argparse
import json
import os
import pathlib
import resource
import selectors
import shutil
import socket
import subprocess
import tempfile
import time
ROOT = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument("--connections", type=int, default=80000)
p.add_argument("--server-nofile", type=int, default=65535)
p.add_argument("--port", type=int, default=3192)
args = p.parse_args()
soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
resource.setrlimit(resource.RLIMIT_NOFILE, (min(hard, args.connections + 1024), hard))
assert resource.getrlimit(resource.RLIMIT_NOFILE)[0] > args.connections
def server_limits():
    resource.setrlimit(resource.RLIMIT_NOFILE, (args.server_nofile, args.server_nofile))
with tempfile.TemporaryDirectory() as tmp:
    db = pathlib.Path(tmp) / "feed.db"
    shutil.copyfile(ROOT.parents[1] / "seed/feed.db", db)
    env = os.environ | dict(SQLITE_PATH=str(db),JWT_SECRET="capacity-test",HOST="127.0.0.1",PORT=str(args.port))
    log = open(pathlib.Path(tmp) / "server.log", "w+")
    proc = subprocess.Popen([str(ROOT / "bin/server")],env=env,preexec_fn=server_limits,stdout=log,stderr=log)
    sockets = []
    start = time.monotonic()
    try:
        time.sleep(.2)
        assert proc.poll() is None
        for n in range(args.connections):
            s = socket.socket()
            s.settimeout(10)
            s.bind((f"127.0.0.{2+n%16}",0))
            s.connect(("127.0.0.1",args.port))
            sockets.append(s)
            if (n+1)%10000 == 0:
                print("opened",n+1,"elapsed",round(time.monotonic()-start,2),flush=True)
        # Every connection must produce a valid response while all others remain open.
        request = b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"
        for base in range(0,len(sockets),64):
            with selectors.DefaultSelector() as sel:
                data = {}
                for s in sockets[base:base+64]:
                    s.sendall(request)
                    s.setblocking(False)
                    sel.register(s,selectors.EVENT_READ)
                    data[s] = b""
                deadline = time.monotonic()+10
                while sel.get_map():
                    assert proc.poll() is None
                    assert time.monotonic()<deadline, "socket response timeout"
                    for key,_ in sel.select(.1):
                        s=key.fileobj
                        part=s.recv(65536)
                        assert part,("connection retired before capacity proof",base,time.monotonic()-start)
                        data[s]+=part
                        if b"\r\n\r\n" not in data[s]:continue
                        h,b=data[s].split(b"\r\n\r\n",1)
                        length=int(next(line.split(b":",1)[1] for line in h.split(b"\r\n") if line.lower().startswith(b"content-length:")))
                        if len(b)<length:continue
                        assert h.startswith(b"HTTP/1.1 200")
                        assert b"connection: close" not in h.lower()
                        assert json.loads(b[:length])["db"]=="ok"
                        sel.unregister(s)
        rss = next(line for line in pathlib.Path(f"/proc/{proc.pid}/status").read_text().splitlines() if line.startswith("VmRSS:"))
        print(json.dumps(dict(live_connections=len(sockets),server_nofile=args.server_nofile,
                              host_kernel=os.uname().release,elapsed_s=time.monotonic()-start,rss=rss,
                              evidence="functional native socket proof; not a constrained k6 score")),flush=True)
    finally:
        for s in sockets:s.close()
        proc.terminate()
        try:proc.wait(10)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
        log.seek(0);print(log.read(),end="")
        log.close()
