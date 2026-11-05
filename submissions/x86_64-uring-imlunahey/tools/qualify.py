#!/usr/bin/env python3
"""Run unchanged bench/load.js with a two-minute warmup and five-minute hold.

Docker separates server and generator resource accounting. They share a private
loopback namespace, so these remain local measurements, not droplet scores.
On Nix, supply the k6 store executable; its runtime is mounted read-only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--users', type=int, default=40000)
p.add_argument('--k6', default=shutil.which('k6'))
p.add_argument('--image', default='redis:8-alpine')
p.add_argument('--label', default='qualification')
a = p.parse_args()
if not a.k6:
    p.error('--k6 or k6 on PATH is required')
k6 = str(Path(a.k6).resolve())
out = ROOT / 'results' / (a.label + '-' + time.strftime('%Y%m%dT%H%M%S'))
out.mkdir(parents=True, exist_ok=False)
server = 'nasm-qualify-' + uuid.uuid4().hex[:10]
clients = []
common = ['-v', f'{REPO}:/repo:ro', '-v', f'{out}:/results']
if Path('/nix/store').exists():
    common += ['-v', '/nix/store:/nix/store:ro']
else:
    common += ['-v', f'{k6}:/tools/k6:ro']
    k6 = '/tools/k6'
evidence = dict(users=a.users, server_limits=dict(cpus=1, memory_bytes=2**31,
    swap_bytes=0, nofile=65535), kernel=os.uname().release,
    binary_sha256=hashlib.sha256((ROOT/'bin/server').read_bytes()).hexdigest(),
    workload_sha256=hashlib.sha256((REPO/'bench/load.js').read_bytes()).hexdigest(),
    topology='separate containers, shared private loopback, shared physical host',
    samples=[], runs=[])

def run(cmd, **kw):
    return subprocess.run(cmd, check=True, text=True, capture_output=True, **kw).stdout

def sample():
    raw = run(['docker', 'exec', server, 'sh', '-c',
        'cat /sys/fs/cgroup/memory.current /sys/fs/cgroup/memory.peak; '
        'cat /sys/fs/cgroup/memory.events /sys/fs/cgroup/cpu.stat; '
        'stat -c "wal_bytes %s" /work/feed.db-wal 2>/dev/null || true'])
    lines = raw.splitlines()
    row = dict(time=time.time(), memory_current=int(lines[0]), memory_peak=int(lines[1]))
    row.update({k: int(v) for k, v in (line.split() for line in lines[2:])})
    evidence['samples'].append(row)
    return row

def workload(name, users, ramp, duration):
    client = server + '-' + name
    clients.append(client)
    summary = f'/results/{name}-summary.json'
    command = ['docker', 'run', '--name', client, '--network', 'container:'+server,
        '--cpuset-cpus', '1-15', '--memory', '18g', '--memory-swap', '18g',
        '--ulimit', 'nofile=524288:524288', '--entrypoint', k6, *common,
        a.image, 'run', '--quiet', '--local-ips', '127.0.0.2-127.0.0.33',
        '--summary-export', summary, '-e', 'BASE_URL=http://127.0.0.1:8080',
        '-e', f'VUS={users}', '-e', f'RAMP_UP={ramp}', '-e', f'DURATION={duration}',
        '/repo/bench/load.js']
    print(f'{name}: {users} users, ramp={ramp}, hold={duration}', flush=True)
    with (out / (name+'.log')).open('w') as log:
        proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        while proc.poll() is None:
            time.sleep(10)
            row = sample()
            print(f'  server memory={row["memory_current"]/2**20:.1f} MiB '
                  f'peak={row["memory_peak"]/2**20:.1f} MiB '
                  f'WAL={row.get("wal_bytes",0)/2**20:.1f} MiB', flush=True)
    state = json.loads(run(['docker', 'inspect', client]))[0]['State']
    data = json.loads((out / (name+'-summary.json')).read_text())
    metrics = data['metrics']
    latency = metrics['http_req_duration']
    failures = metrics['http_req_failed']['value']
    checks = metrics['checks']['value']
    passed = (proc.returncode == 0 and not state['OOMKilled'] and
              latency['p(95)'] < 500 and latency['p(99)'] < 1000 and
              failures < .01 and checks == 1)
    result = dict(name=name, users=users, ramp=ramp, hold=duration, passed=passed,
        exit_code=proc.returncode, client_state=state, p95_ms=latency['p(95)'],
        p99_ms=latency['p(99)'], failure_rate=failures, checks_rate=checks,
        requests=metrics['http_reqs']['count'])
    evidence['runs'].append(result)
    print(json.dumps(result), flush=True)
    if not passed:
        raise RuntimeError(f'{name} failed; see {out}')

try:
    run(['docker', 'run', '-d', '--name', server, '--network', 'none',
        '--cpus', '1', '--cpuset-cpus', '0', '--memory', '2g', '--memory-swap', '2g',
        '--ulimit', 'nofile=65535:65535', '--security-opt', 'seccomp=unconfined',
        '--entrypoint', 'sh', *common, '-e', 'SQLITE_PATH=/work/feed.db',
        '-e', 'JWT_SECRET=twelve-dollar-challenge', '-e', 'HOST=127.0.0.1',
        '-e', 'PORT=8080', a.image, '-c',
        'mkdir -p /work && cp /repo/seed/feed.db /work/feed.db && '
        'exec /repo/submissions/x86_64-uring-imlunahey/bin/server'])
    for _ in range(100):
        check = subprocess.run(['docker', 'exec', server, 'wget', '-qO-',
            'http://127.0.0.1:8080/health'], capture_output=True)
        if check.returncode == 0 and b'"db":"ok"' in check.stdout:
            break
        time.sleep(.1)
    else:
        raise RuntimeError('server did not become healthy')
    evidence['server_inspect'] = json.loads(run(['docker', 'inspect', server]))[0]
    workload('warmup', 1000, '1s', '2m')
    workload('capacity', a.users, '60s', '5m')
    sample()
    if any(row.get('oom_kill', 0) for row in evidence['samples']):
        raise RuntimeError('server recorded an OOM kill')
    # Graceful shutdown must finish before the persistence check.
    run(['docker', 'stop', '--time', '15', server])
    state = json.loads(run(['docker', 'inspect', server]))[0]['State']
    evidence['server_final_state'] = state
    if state['ExitCode'] != 0 or state['OOMKilled']:
        raise RuntimeError(f'unclean server shutdown: {state}')
    run(['docker', 'cp', server+':/work/feed.db', str(out/'feed.db')])
    import sqlite3
    with sqlite3.connect(out/'feed.db') as db:
        integrity = db.execute('PRAGMA integrity_check').fetchone()[0]
        evidence['integrity_check'] = integrity
        assert integrity == 'ok', integrity
    (out/'feed.db').unlink()
    evidence['passed'] = True
finally:
    (out/'evidence.json').write_text(json.dumps(evidence, indent=2)+'\n')
    log = subprocess.run(['docker', 'logs', server], capture_output=True, text=True)
    (out/'server.log').write_text(log.stdout+log.stderr)
    for container in [*clients, server]:
        subprocess.run(['docker', 'rm', '-f', container], capture_output=True)
    print(f'Evidence: {out}', flush=True)
