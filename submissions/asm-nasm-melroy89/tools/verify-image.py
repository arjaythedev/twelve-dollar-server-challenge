#!/usr/bin/env python3
"""Packaged NASM default/fallback startup and unchanged official correctness."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import urllib.request
import uuid
here=Path(__file__).resolve().parents[1]
root=here.parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--image',default='twelve-nasm-candidate:local')
parser.add_argument('--label',default='candidate')
args=parser.parse_args()
if not args.label.replace('-','').replace('_','').isalnum():parser.error('invalid label')
results=[]
def command(argv):return subprocess.run(argv,text=True,capture_output=True,check=True).stdout
for backend,flags in [('uring',[]),('epoll',['--epoll'])]:
    name='nasm-package-'+uuid.uuid4().hex[:10]
    try:
        begun=time.monotonic()
        command(['docker','run','-d','--name',name,'--network=host','--cpus=1','--memory=2g','--memory-swap=2g','--ulimit','nofile=65535:65535','--security-opt','seccomp=unconfined','-e','JWT_SECRET=twelve-dollar-challenge','-e','HOST=127.0.0.1','-e','PORT=3190','-e','SQLITE_PATH=/tmp/feed.db','-v',str(root/'seed')+':/seed:ro',args.image,'bash','-c','cp /seed/feed.db /tmp/feed.db && '+('exec bash start.sh' if not flags else 'exec bin/server "$@"'),'nasm',*flags])
        while time.monotonic()-begun<60:
            try:
                with urllib.request.urlopen('http://127.0.0.1:3190/health',timeout=.5) as r:
                    if r.status==200:break
            except OSError:time.sleep(.05)
        else:raise RuntimeError('startup exceeded 60 seconds')
        healthy=time.monotonic()-begun
        identity=json.loads(command(['docker','exec',name,'cat','/app/build/identity.json']))
        assert identity['io_stats'] is False
        assert command(['docker','exec',name,'readlink','/proc/1/exe']).strip()=='/app/bin/server'
        assert command(['docker','exec',name,'id','-u']).strip()!='0'
        test=subprocess.run(['bash',str(root/'test/test.sh'),'http://127.0.0.1:3190'],text=True,capture_output=True,timeout=60)
        (here/'results'/(args.label+'-'+backend+'-official.log')).write_text(test.stdout+test.stderr)
        assert test.returncode==0 and 'passed=42 failed=0' in test.stdout,test.stdout+test.stderr
        logs=subprocess.run(['docker','logs',name],text=True,capture_output=True,check=True)
        assert ('raw io_uring' if backend=='uring' else 'epoll, WAL/NORMAL') in logs.stdout+logs.stderr
        assert 'IO_STATS' not in logs.stdout+logs.stderr
        results.append(dict(backend=backend,official='42/42',healthy_seconds=healthy,identity=identity,image=command(['docker','inspect','--format','{{.Image}}',name]).strip(),foreground=True,normal_user=True))
        print('PASS',backend,'42/42',round(healthy,3),'seconds',flush=True)
    finally:subprocess.run(['docker','rm','-f',name],capture_output=True)
(here/'results'/(args.label+'-package-evidence.json')).write_text(json.dumps(results,indent=2)+'\n')
