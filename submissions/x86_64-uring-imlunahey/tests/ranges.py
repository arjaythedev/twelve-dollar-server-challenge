#!/usr/bin/env python3
"""Differential tests for bounded range membership, ordering, skew and fallbacks."""
import hashlib,json,os,pathlib,sqlite3,subprocess,tempfile,time,urllib.request
ROOT=pathlib.Path(__file__).resolve().parents[1]
REPO=ROOT.parents[1]
cases=[]
with tempfile.TemporaryDirectory() as tmp:
    db=pathlib.Path(tmp)/'feed.db'
    for fixture in ['empty','dense','interleaved','sparse','skew','signed']:
        for txn in ['per-request','writes','batch']:
            if db.exists():db.unlink()
            with sqlite3.connect(db) as c:
                c.executescript((REPO/'schema.sql').read_text())
                c.executemany('INSERT INTO users(id,username) VALUES(?,?)',[(n,'café "😀" '+str(n)) for n in range(1,4201 if fixture=='skew' else 51)])
                for n in range(1,0 if fixture=='empty' else 41):
                    ident=n*1000 if fixture=='sparse' else n
                    if fixture=='signed':ident=n-21
                    # Twenty selected odd IDs, with ties and timestamp ordering opposite ID.
                    day=30-n//2 if n%2 else 1
                    c.execute('INSERT INTO posts(id,user_id,body,created_at) VALUES(?,?,?,?)',(ident,1,'hello 😀\n"\\'+str(n),f'2026-01-{day:02d}T00:00:00.000Z'))
                    count=4200 if fixture=='skew' and n==2 else n%7
                    c.executemany('INSERT INTO likes(user_id,post_id) VALUES(?,?)',[(u,ident) for u in range(1,count+1)])
                rows=c.execute('SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20').fetchall()
                expected={'posts':[dict(zip(['id','body','created_at','author','like_count'],r)) for r in rows]}
            c.close()
            env=os.environ|dict(SQLITE_PATH=str(db),JWT_SECRET='range-test',HOST='127.0.0.1',PORT='3196')
            with open(pathlib.Path(tmp)/'log','w+') as log:
                p=subprocess.Popen([str(ROOT/'bin/server'),'--query=scan-ranges',f'--transaction={txn}'],env=env,stdout=log,stderr=log)
                try:
                    for attempt in range(100):
                        assert p.poll() is None,(fixture,txn,'server exited')
                        try:
                            with urllib.request.urlopen('http://127.0.0.1:3196/health',timeout=.2):break
                        except OSError:time.sleep(.02)
                    for _ in range(3):
                        with urllib.request.urlopen('http://127.0.0.1:3196/feed') as r:actual=json.load(r)
                        assert actual==expected,(fixture,txn,actual,expected)
                    cases.append([fixture,txn]);print('PASS',fixture,txn,flush=True)
                finally:
                    p.terminate();p.wait(5)
            for suffix in ['-wal','-shm']:
                x=pathlib.Path(str(db)+suffix)
                if x.exists():x.unlink()
(ROOT/'results/ranges-differential.json').write_text(json.dumps(dict(binary_sha256=hashlib.sha256((ROOT/'bin/server').read_bytes()).hexdigest(),cases=cases),indent=2)+'\n')
