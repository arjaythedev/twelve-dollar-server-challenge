#!/usr/bin/env python3
"""Compare both generated text lowerings in one SHA, with identical serialized output."""
import json, os, pathlib, subprocess, sys, time, statistics
root=pathlib.Path('/root/twelve/opt-audit-1024/metal')
sha=sys.argv[1]
out=root/'results'/f'text-{sha}-{time.time_ns()}'
out.mkdir(parents=True)
rows=[]
for label,body in [('short','typed code says hello'),('ascii500','x'*500),('unicode500','😀'*500)]:
    for kind in ['general','plain','plain','general','general','plain']:
        subprocess.run(['python3',str(root/'source/tests/metal/wait_quiet.py'),'2'],check=True,stdout=subprocess.DEVNULL)
        cmd=['taskset','-c','0',str(root/'artifacts'/sha/'micro'),'serialize-'+kind,'1','2000000']
        p=subprocess.Popen(cmd,env=dict(os.environ,BENCH_TEXT=body),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        overlap=set()
        while p.poll() is None:
            for proc in pathlib.Path('/proc').glob('[0-9]*/cmdline'):
                try: exe=proc.read_bytes().split(b'\0')[0]
                except OSError: continue
                if exe==b'/root/twelve/loadgen/loadgen' or exe.endswith(b'/cc1'):overlap.add(proc.parent.name)
            time.sleep(.05)
        stdout,stderr=p.communicate()
        if p.returncode: raise RuntimeError(stderr)
        row=dict(sha=sha,label=label,kind=kind,ns_per_request=float(stdout.split('ns/request=')[1].split()[0]),overlap=sorted(overlap))
        rows.append(row);(out/'summary.json').write_text(json.dumps(rows,indent=2))
        print(json.dumps(row),flush=True)
    print('MEDIAN',label,{k:statistics.median(r['ns_per_request'] for r in rows if r['label']==label and r['kind']==k and not r['overlap']) for k in ['general','plain']},flush=True)
print('RESULTS',out,flush=True)
