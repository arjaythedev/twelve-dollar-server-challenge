#!/usr/bin/env python3
"""Interleaved thread-CPU measurements of SHA-keyed executables; one source tree."""
import json, os, pathlib, shutil, subprocess, sys, time, statistics, base64, hashlib, hmac
root=pathlib.Path('/root/twelve/opt-audit-1024/metal')
a,b=sys.argv[1:3]
long_claims='--long-claims' in sys.argv[3:]
cases=[x for x in sys.argv[3:] if x != '--long-claims'] or ['mixed:1:300000','mixed:32:30000','post:1:400000']
out=root/'results'/f'pair-{a[:8]}-{b[:8]}-{"longclaims-" if long_claims else ""}{time.time_ns()}'
out.mkdir(parents=True)
db=out/'feed.db'
shutil.copyfile('/root/twelve/challenge/seed/feed.db',db)
tokens=json.loads(pathlib.Path('/root/twelve/challenge/seed/tokens.json').read_text())
token=tokens[0]['token']
if long_claims:
    def b64(x): return base64.urlsafe_b64encode(x).rstrip(b'=')
    payload={'sub':'1','username':('"\\\n\t'*60),'exp':int(time.time())+3600}
    msg=b64(b'{"alg":"HS256"}')+b'.'+b64(json.dumps(payload).encode())
    token=(msg+b'.'+b64(hmac.new(b'twelve-dollar-challenge',msg,hashlib.sha256).digest())).decode()
env=dict(os.environ,SQLITE_PATH=str(db),BENCH_AUTH='Bearer '+token)
rows=[]
def external():
    pids=[]
    for p in pathlib.Path('/proc').glob('[0-9]*/cmdline'):
        try: argv=p.read_bytes().split(b'\0')
        except OSError: continue
        if argv[0]==b'/root/twelve/loadgen/loadgen' or argv[0].endswith(b'/cc1'): pids.append(p.parent.name)
    return pids
try:
    for case in cases:
        kind,batch,rounds=case.split(':')
        for sha in [a,b,b,a,a,b]:
            subprocess.run(['python3',str(root/'source/tests/metal/wait_quiet.py'),'2'],check=True,stdout=subprocess.DEVNULL)
            p=subprocess.Popen(['taskset','-c','0',str(root/'artifacts'/sha/'micro'),kind,batch,rounds],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            overlap=set()
            while p.poll() is None:
                overlap.update(external()); time.sleep(.05)
            stdout,stderr=p.communicate()
            if p.returncode: raise RuntimeError(stderr)
            row=dict(sha=sha,kind=kind,batch=int(batch),ns_per_request=float(stdout.split('ns/request=')[1].split()[0]),overlap=sorted(overlap),output=stdout.strip())
            rows.append(row); (out/'summary.json').write_text(json.dumps(rows,indent=2))
            print(json.dumps(row),flush=True)
    for case in cases:
        kind,batch,_=case.split(':')
        vals={sha:[r['ns_per_request'] for r in rows if r['sha']==sha and r['kind']==kind and r['batch']==int(batch) and not r['overlap']] for sha in [a,b]}
        med={sha:statistics.median(v) if v else None for sha,v in vals.items()}
        print('MEDIAN',case,json.dumps(med),flush=True)
    print('RESULTS',out,flush=True)
finally:
    for p in out.glob('feed.db*'):p.unlink()
