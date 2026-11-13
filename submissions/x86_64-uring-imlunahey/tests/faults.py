#!/usr/bin/env python3
"""Test-only linker wrapping: failed COMMIT and transaction-wide automatic rollback."""
import argparse
import base64
import concurrent.futures
import hashlib
import hmac
import http.client
import json
import os
import pathlib
import shutil
import sqlite3
import subprocess
import tempfile
import time
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--server-arg",action="append",default=[])
args=parser.parse_args()
ROOT=pathlib.Path(__file__).resolve().parents[1]
seed=ROOT.parents[1]/"seed/feed.db"
secret=b"fault-test"
encode=lambda x:base64.urlsafe_b64encode(json.dumps(x).encode()).rstrip(b"=")
raw=encode({"alg":"HS256"})+b"."+encode({"sub":"1","username":"fault","exp":int(time.time())+3600})
auth=(raw+b"."+base64.urlsafe_b64encode(hmac.digest(secret,raw,"sha256")).rstrip(b"=")).decode()
def request(body):
    c=http.client.HTTPConnection("127.0.0.1",3196,timeout=10)
    c.request("POST","/posts",json.dumps({"body":body}),{"Authorization":"Bearer "+auth,"Content-Type":"application/json"})
    r=c.getresponse();result=(r.status,json.loads(r.read()));c.close();return result
def read_feed(_):
    c=http.client.HTTPConnection("127.0.0.1",3196,timeout=10)
    c.request("GET","/feed")
    r=c.getresponse();result=(r.status,json.loads(r.read()));c.close();return result
for automatic in [False,True]:
    opts=["-DROLLBACK_TEST=1"] if automatic else []
    subprocess.run(["nasm","-f","elf64",*opts,"tests/fault.asm","-o","build/fault.o"],cwd=ROOT,check=True)
    sqlite=ROOT/"build/sqlite-diagnostic.o"
    assert sqlite.exists(),"build.sh diagnostic is required first"
    subprocess.run(["gcc","-no-pie","-flto","-Wl,--wrap=sqlite3_step","build/server.o","build/json.o",
                    "build/crypto.o","build/fault.o",str(sqlite),"-lm","-o","build/server-fault"],
                    cwd=ROOT,check=True)
    for mode in ["per-request","writes","batch"]:
        with tempfile.TemporaryDirectory() as tmp:
            db=pathlib.Path(tmp)/"feed.db";shutil.copyfile(seed,db)
            env=os.environ|dict(SQLITE_PATH=str(db),JWT_SECRET=secret.decode(),HOST="127.0.0.1",PORT="3196")
            p=subprocess.Popen([str(ROOT/"build/server-fault"),f"--transaction={mode}","--recv=multi","--wait-us=250","--batch=32",*args.server_arg],env=env)
            try:
                time.sleep(.2)
                bodies=[f"fault-case-{i}" for i in range(32)]
                with concurrent.futures.ThreadPoolExecutor(max_workers=40) as pool:
                    writes=[pool.submit(request,body) for body in bodies]
                    reads=[pool.submit(read_feed,n) for n in range(8)]
                    results=[f.result() for f in writes]
                    read_results=[f.result() for f in reads]
                assert all(status in (201,500) for status,_ in results),results
                assert any(status==500 for status,_ in results),"fault did not exercise a request"
                successes=[(body,r["post"]["id"]) for body,(status,r) in zip(bodies,results) if status==201]
                # Test recovery after the synthetic one-time failure.
                assert request("after-fault")[0]==201
                p.kill();p.wait(5)
                with sqlite3.connect(db) as c:
                    for body,ident in successes:
                        assert c.execute("SELECT body FROM posts WHERE id=?",(ident,)).fetchone()==(body,)
                    for status, response in read_results:
                        assert status in (200,500)
                        if status==200:
                            for post in response["posts"]:
                                if post["body"].startswith("fault-case-"):
                                    assert c.execute("SELECT body FROM posts WHERE id=?",(post["id"],)).fetchone()==(post["body"],),"successful read exposed a rolled-back write"
                    assert c.execute("PRAGMA integrity_check").fetchone()==("ok",)
                c.close()
                print("PASS",mode,"automatic rollback" if automatic else "COMMIT failure",
                      "acknowledged rows preserved",len(successes),flush=True)
            finally:
                if p.poll() is None:p.terminate()
                try:p.wait(5)
                except subprocess.TimeoutExpired:p.kill();p.wait()
