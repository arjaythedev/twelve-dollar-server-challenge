#!/usr/bin/env python3
"""Fresh-seed constrained CPU screens; substitute load is not a capacity score."""
import argparse
import concurrent.futures
import json
import hashlib
import os
import subprocess
import time
import urllib.request
import uuid
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--duration', type=int, default=10)
p.add_argument('--label', required=True)
p.add_argument('--image', default='twelve-nasm-qualification:local')
p.add_argument('--variant', action='append', help='name:space-separated server flags')
a = p.parse_args()
if not a.label.replace("-", "").replace("_", "").isalnum(): p.error("invalid label")
if not 1 <= a.duration <= 300: p.error("duration must be 1..300 seconds")
here = Path(__file__).resolve().parents[1]
root = here.parents[1]
results = here / 'results'
results.mkdir(exist_ok=True)
def run(argv):
    r = subprocess.run(argv, text=True, capture_output=True)
    if r.returncode: raise RuntimeError(f'{argv[0]} failed: {r.stdout} {r.stderr}')
    return r.stdout

def trial(spec):
    label, _, flags = spec.partition(':')
    name = 'nasm-screen-' + uuid.uuid4().hex[:10]
    try:
        run(['docker', 'run', '-d', '--name', name, '--network=host', '--cpus=1', '--memory=2g', '--memory-swap=2g', '--ulimit', 'nofile=65535:65535', '--security-opt', 'seccomp=unconfined', '-e', 'JWT_SECRET=twelve-dollar-challenge', '-e', 'HOST=127.0.0.1', '-e', 'PORT=3100', '-e', 'SQLITE_PATH=/tmp/feed.db', '-v', str(root / 'seed') + ':/seed:ro', a.image, 'bash', '-c', 'cp /seed/feed.db /tmp/feed.db && exec bin/server "$@"', 'nasm', *flags.split()])
        for _ in range(600):
            try:
                with urllib.request.urlopen('http://127.0.0.1:3100/health', timeout=1): break
            except OSError: time.sleep(.1)
        else: raise RuntimeError('startup timeout')
        def cpu():
            return dict(line.split() for line in run(['docker', 'exec', name, 'cat', '/sys/fs/cgroup/cpu.stat']).splitlines())
        start = cpu()
        begun = time.monotonic()
        def client(i):
            return json.loads(run(['node', str(here / 'tools/saturation.js'), str(root / 'seed/tokens.json'), 'http://127.0.0.1:3100', str(a.duration), str(i)]))
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            clients = list(pool.map(client, range(4)))
        elapsed = time.monotonic() - begun
        end = cpu()
        total = sum(c['requests']['total'] for c in clients)
        used = (int(end['usage_usec']) - int(start['usage_usec'])) / 1e6
        result = dict(variant=label, flags=flags.split(), elapsed=elapsed, requests=total, requests_per_second=sum(c["requests"]["total"] / c["duration"] for c in clients), cpu_seconds=used, requests_per_cpu_second=total / used, clients=clients,
                      memory_peak=int(run(['docker', 'exec', name, 'cat', '/sys/fs/cgroup/memory.peak'])),
                      identity=json.loads(run(['docker', 'exec', name, 'cat', '/app/build/identity.json'])),
                      kind='same-host substitute-generator saturation diagnostic',
                      image=run(['docker','inspect','--format','{{.Image}}',name]).strip(),
                      host_kernel=os.uname().release, cpu_quota=1, memory_limit=2147483648, swap=0, nofile=65535,
                      node_version=run(['node', '--version']).strip(),
                      autocannon_version=run(['node', '-p', "require('/usr/lib/node_modules/autocannon/package.json').version"]).strip(),
                      generator_sha256=hashlib.sha256((here / 'tools/saturation.js').read_bytes()).hexdigest())
        print(json.dumps({k: v for k, v in result.items() if k not in ('identity', 'clients')}), flush=True)
        return result
    finally:
        subprocess.run(['docker', 'rm', '-f', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

all_results = []
try:
    for variant in a.variant or ['baseline:', 'writes:--transaction=writes', 'batch:--transaction=batch']:
        all_results.append(trial(variant))
finally:
    (results / (a.label + '.json')).write_text(json.dumps(all_results, indent=2) + '\n')
