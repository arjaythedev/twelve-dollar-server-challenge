#!/usr/bin/env python3
"""Mixed-load saturation and optional sampled profiling in a bounded server.

Run under nix-shell -p python3 wrk perf on Nix. The target binary must already
be built. Profiled timings are diagnostic, not comparable throughput results.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--label', default='mixed')
p.add_argument('--seconds', type=int, default=30)
p.add_argument('--connections', type=int, default=128)
p.add_argument('--server-arg', action='append', default=[])
p.add_argument('--profile', action='store_true')
a = p.parse_args()
if a.seconds < 5 or a.connections < 4:
    p.error('use at least five seconds and four connections')
wrk = shutil.which('wrk')
perf = shutil.which('perf')
if not wrk or (a.profile and not perf):
    p.error('wrk (and perf for --profile) must be on PATH')
wrk = str(Path(wrk).resolve())
if perf:
    perf = str(Path(perf).resolve())
out = ROOT/'results'/(a.label+'-'+time.strftime('%Y%m%dT%H%M%S'))
out.mkdir(parents=True, exist_ok=False)
name = 'nasm-profile-'+uuid.uuid4().hex[:10]
image = 'redis:8-alpine'
mounts = ['-v', f'{REPO}:/repo:ro', '-v', f'{out}:/results']
if Path('/nix/store').exists():
    mounts += ['-v', '/nix/store:/nix/store:ro']
else:
    raise SystemExit('This local profiling runner currently requires Nix runtime paths')
names = [name]

def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout

def stats():
    text = run(['docker', 'exec', name, 'sh', '-c',
        'cat /sys/fs/cgroup/cpu.stat /sys/fs/cgroup/memory.events; '
        'printf "memory_peak "; cat /sys/fs/cgroup/memory.peak; '
        'stat -c "wal_bytes %s" /work/feed.db-wal 2>/dev/null || true'])
    return {key: int(value) for key, value in (line.split() for line in text.splitlines())}

def load(label, seconds):
    client = name+'-'+label
    names.append(client)
    return ['docker', 'run', '--name', client, '--network', 'container:'+name,
        '--cpuset-cpus', '1-4', '--memory', '2g', '--memory-swap', '2g',
        '--entrypoint', wrk, *mounts, image, '-t4', f'-c{a.connections}',
        f'-d{seconds}s', '--latency', '--timeout', '10s', '-s',
        '/repo/submissions/x86_64-uring-imlunahey/tools/mixed.lua',
        'http://127.0.0.1:8080', '--', '/repo/seed/tokens.json']

try:
    run(['docker', 'run', '-d', '--name', name, '--network', 'none',
        '--cpus', '1', '--cpuset-cpus', '0', '--memory', '2g', '--memory-swap', '2g',
        '--ulimit', 'nofile=65535:65535', '--security-opt', 'seccomp=unconfined',
        '--entrypoint', 'sh', *mounts, '-e', 'SQLITE_PATH=/work/feed.db',
        '-e', 'JWT_SECRET=twelve-dollar-challenge', '-e', 'HOST=127.0.0.1',
        '-e', 'PORT=8080', image, '-c',
        'mkdir -p /work && cp /repo/seed/feed.db /work/feed.db && '
        'exec /repo/submissions/x86_64-uring-imlunahey/bin/server "$@"',
        'server', *a.server_arg])
    for _ in range(100):
        health = subprocess.run(['docker', 'exec', name, 'wget', '-qO-',
            'http://127.0.0.1:8080/health'], capture_output=True)
        if health.returncode == 0 and b'"db":"ok"' in health.stdout:
            break
        time.sleep(.1)
    else:
        raise RuntimeError('server startup failed')
    (out/'warmup.log').write_text(run(load('warmup', 5)))
    before = stats()
    with (out/'measured.log').open('w') as log:
        measured = subprocess.Popen(load('measured', a.seconds), stdout=log,
                                    stderr=subprocess.STDOUT)
        if a.profile:
            profiler = name+'-perf'
            names.append(profiler)
            command = ['docker', 'run', '--name', profiler, '--pid', 'container:'+name,
                '--cap-add', 'PERFMON', '--cap-add', 'SYS_PTRACE',
                '--security-opt', 'seccomp=unconfined', '--entrypoint', perf,
                *mounts, image, 'record', '-F', '199', '-e', 'cycles',
                '-p', '1', '-o', '/results/perf.data', '--', 'sleep', str(a.seconds-2)]
            capture = subprocess.run(command, capture_output=True, text=True)
            (out/'perf.log').write_text(capture.stdout+capture.stderr)
            capture.check_returncode()
        measured.wait()
        assert measured.returncode == 0, measured.returncode
    after = stats()
    output = (out/'measured.log').read_text()
    print(output, flush=True)
    rps = float(re.search(r'Requests/sec:\s+([\d.]+)', output)[1])
    p95, p99, requests, invalid = map(int, re.search(
        r'p95_us=(\d+) p99_us=(\d+) requests=(\d+) invalid=(\d+)', output).groups())
    result = dict(label=a.label, profiled=a.profile, arguments=a.server_arg,
        binary_sha256=hashlib.sha256((ROOT/'bin/server').read_bytes()).hexdigest(),
        requests=requests, rps=rps, p95_us=p95, p99_us=p99, invalid=invalid,
        cpu_us_per_request=(after['usage_usec']-before['usage_usec'])/requests,
        before=before, after=after, duration=a.seconds, connections=a.connections)
    (out/'evidence.json').write_text(json.dumps(result, indent=2)+'\n')
    assert invalid == 0 and 'Socket errors' not in output and 'Non-2xx' not in output
    if a.profile:
        report = run(['docker', 'run', '--rm', '--entrypoint', 'sh', *mounts,
            image, '-c', f'{perf} report -i /results/perf.data --stdio '
            '--no-children --percent-limit 0.5 && chmod 644 /results/perf.data'])
        (out/'profile.txt').write_text(report)
        print(report, flush=True)
finally:
    logs = subprocess.run(['docker', 'logs', name], capture_output=True, text=True)
    (out/'server.log').write_text(logs.stdout+logs.stderr)
    for container in reversed(names):
        subprocess.run(['docker', 'rm', '-f', container], capture_output=True)
    print(f'Evidence: {out}', flush=True)
