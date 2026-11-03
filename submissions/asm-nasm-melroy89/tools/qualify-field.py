#!/usr/bin/env python3
"""Clean Ubuntu root-install/normal-build and direct-mode official correctness gate.
Only run after reviewing a contender's installer/build/start and declared mode.
Builds occur in Docker; no host installation or contender source modification.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time
import urllib.request
import uuid
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--pr', type=int, required=True)
p.add_argument('--direct', action='store_true', help='reviewed direct mode; Nginx candidates need a different packaging harness')
p.add_argument('--port', type=int, default=3101)
a = p.parse_args()
if not a.direct: p.error('this harness supports reviewed direct-mode contenders only')
here = Path(__file__).resolve().parents[1]
root = here.parents[1]
paths = list((here / 'results/field').glob(str(a.pr) + '-*/manifest.json'))
if len(paths) != 1: raise SystemExit('need exactly one pinned manifest')
manifest_path = paths[0]
m = json.loads(manifest_path.read_text())
folders = {Path(x).parts[1] for x in m['source_files']}
if len(folders) != 1: raise SystemExit('submission directory needs manual review')
folder = next(iter(folders))
if not re.fullmatch(r'[A-Za-z0-9_.-]+', folder): raise SystemExit('invalid folder')
context = manifest_path.parent / 'source'
for path, expected in m['source_files'].items():
    if hashlib.sha256((context / path).read_bytes()).hexdigest() != expected:
        raise SystemExit('pinned source changed: ' + path)
identity = dict(pull_request=a.pr, head_sha=m['sha'], source_sha256=m['source_files'], binary_sha256=None,
                boundary='Pinned image identity includes binaries; individual binary hash not yet recorded')
(context / 'qualification-identity.json').write_text(json.dumps(identity, indent=2) + '\n')
dockerfile = manifest_path.parent / 'Qualification.Dockerfile'
dockerfile.write_text('FROM ubuntu:24.04\nWORKDIR /app\nCOPY submissions/' + folder + '/ /app/\nRUN bash install.sh\nRUN useradd --create-home --uid 1001 runner && chown -R runner:runner /app\nUSER runner\nRUN bash build.sh\nCOPY --chown=runner:runner qualification-identity.json /qualification-identity.json\nRUN mkdir -p /app/build && cp /qualification-identity.json /app/build/identity.json\nCMD ["bash", "start.sh"]\n')
image = f'twelve-field-{a.pr}:{m["sha"][:12]}'
report = dict(pr=a.pr, head_sha=m['sha'], mode='direct', source_verified=True, host_kernel=os.uname().release,
              server_limits=dict(cpus=1,memory=2147483648,swap=0,nofile=65535), build='pending', official_tests='not run')
def command(argv, **kw): return subprocess.run(argv, check=True, text=True, **kw)
name = 'field-qualification-' + uuid.uuid4().hex[:10]
try:
    with (manifest_path.parent / 'build.log').open('w') as log:
        built = subprocess.run(['docker', 'build', '-f', str(dockerfile), '-t', image, str(context)], stdout=log, stderr=subprocess.STDOUT, timeout=1800)
    report['build'] = 'passed' if built.returncode == 0 else 'failed'
    if built.returncode: raise RuntimeError('build failed; inspect build.log')
    report['image'] = command(['docker', 'image', 'inspect', '--format', '{{.Id}}', image], capture_output=True).stdout.strip()
    command(['docker','run','-d','--name',name,'--network=host','--cpus=1','--memory=2g','--memory-swap=2g','--ulimit','nofile=65535:65535','--security-opt','seccomp=unconfined',
             '-e','JWT_SECRET=twelve-dollar-challenge','-e','HOST=127.0.0.1','-e',f'PORT={a.port}','-e','SQLITE_PATH=/tmp/feed.db','-v',str(root / 'seed') + ':/seed:ro',image,
             'bash','-c','cp /seed/feed.db /tmp/feed.db && exec bash start.sh'],stdout=subprocess.DEVNULL)
    begun=time.monotonic()
    while time.monotonic()-begun<60:
        try:
            with urllib.request.urlopen(f'http://127.0.0.1:{a.port}/health',timeout=.5) as response:
                if response.status==200: break
        except OSError: time.sleep(.1)
    else: raise RuntimeError('not healthy within 60 seconds')
    report['healthy_seconds']=time.monotonic()-begun
    report['runtime_uid']=command(['docker','exec',name,'id','-u'],capture_output=True).stdout.strip()
    result=subprocess.run(['bash',str(root / 'test/test.sh'),f'http://127.0.0.1:{a.port}'],text=True,capture_output=True,timeout=60)
    (manifest_path.parent / 'official-tests.log').write_text(result.stdout+result.stderr)
    report['official_tests']='passed' if result.returncode==0 and 'passed=42 failed=0' in result.stdout else 'failed'
    report['official_test_exit_code']=result.returncode
    report['memory_peak_bytes']=int(command(['docker','exec',name,'cat','/sys/fs/cgroup/memory.peak'],capture_output=True).stdout)
    print(json.dumps(report),flush=True)
finally:
    subprocess.run(['docker','rm','-f',name],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    (manifest_path.parent / 'qualification.json').write_text(json.dumps(report,indent=2)+'\n')
