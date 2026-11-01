#!/usr/bin/env python3
"""Official correctness and reference-query differential matrix, using disposable files."""
import argparse
import hashlib
import itertools
import json
import os
import pathlib
import shutil
import sqlite3
import subprocess
import tempfile
import time
import urllib.request
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--server-arg",action="append",default=[])
args=parser.parse_args()
ROOT=pathlib.Path(__file__).resolve().parents[1]
REPO=ROOT.parents[1]
results=[]
def start(db, flags, log):
    env=os.environ|dict(SQLITE_PATH=str(db),JWT_SECRET="twelve-dollar-challenge",HOST="127.0.0.1",PORT="3194")
    p=subprocess.Popen([str(ROOT/"bin/server"),*flags],env=env,stdout=log,stderr=log)
    for _ in range(100):
        assert p.poll() is None,"server exited"
        try:
            with urllib.request.urlopen("http://127.0.0.1:3194/health",timeout=.1) as r:
                if r.status==200:return p
        except OSError:time.sleep(.01)
    p.terminate();p.wait(5)
    raise AssertionError("startup timeout")
def stop(p):
    p.terminate()
    try:p.wait(5)
    except subprocess.TimeoutExpired:p.kill();p.wait()
with tempfile.TemporaryDirectory() as tmp:
    tmp=pathlib.Path(tmp)
    for query,serializer,txn in itertools.product(["reference","selected","range"],["sql","nasm","hybrid"],["per-request","writes","batch"]):
        flags=[f"--query={query}",f"--json={serializer}",f"--transaction={txn}","--recv=multi",*args.server_arg]
        db=tmp/"feed.db";shutil.copyfile(REPO/"seed/feed.db",db)
        with open(tmp/"server.log","w+") as log:
            p=start(db,flags,log)
            try:
                r=subprocess.run(["bash",str(REPO/"test/test.sh"),"http://127.0.0.1:3194"],capture_output=True,text=True,timeout=60)
                assert r.returncode==0,(flags,r.stdout,r.stderr)
                results.append(dict(flags=flags,official="42/42"))
                print("PASS",*flags,flush=True)
            finally:stop(p)
        # Fresh tiny fixture with ordering contrary to IDs, Unicode and skewed likes.
        db.unlink()
        with sqlite3.connect(db) as c:
            c.executescript((REPO/"schema.sql").read_text())
            c.executemany("INSERT INTO users(id,username) VALUES(?,?)",[(n,f'author {n} café "quoted"') for n in range(1,51)])
            for n in range(1,41):
                ident=n if n%2 else n*1000
                created=f"2026-01-{1+(40-n)//2:02d}T00:00:00.000Z"
                c.execute("INSERT INTO posts(id,user_id,body,created_at) VALUES(?,?,?,?)",(ident,1+n%50,f'body {n} 😀\n\\',created))
                c.executemany("INSERT INTO likes(user_id,post_id) VALUES(?,?)",[(u,ident) for u in range(1,51 if n==1 else 3)])
            rows=c.execute("""SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id)
                            FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20""").fetchall()
            expected=[dict(zip(["id","body","created_at","author","like_count"],row)) for row in rows]
        c.close()
        with open(tmp/"server.log","w+") as log:
            p=start(db,flags,log)
            try:
                with urllib.request.urlopen("http://127.0.0.1:3194/feed") as r:actual=json.load(r)
                assert actual=={"posts":expected},(flags,actual,expected)
                assert all(list(x)==["id","body","created_at","author","like_count"] for x in actual["posts"])
            finally:stop(p)
        db.unlink()
    (ROOT/"results").mkdir(exist_ok=True)
    (ROOT/"results/correctness-matrix.json").write_text(json.dumps(dict(binary_sha256=hashlib.sha256((ROOT/"bin/server").read_bytes()).hexdigest(),results=results,differential="sparse/reversed/tied/skewed/Unicode"),indent=2)+"\n")
print("PASS: 27 official configurations and 27 reference-query differential fixtures")
