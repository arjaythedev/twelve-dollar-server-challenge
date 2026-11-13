#!/usr/bin/env python3
"""Run the pinned mjc generator with independently bounded server/client cgroups.

Build deps/twelve-dollar-benchmark at the recorded revision into deps/mjc-build
using Rust 1.99.1 on Ubuntu 24.04 first. Invoke the native binary directly: the
upstream shell launcher changes host sysctls. This runner changes none.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parents[1]
REVISION = 'e31cd6285704b436422c2fc100d3beb10e558f6c'
GENERATOR = ROOT/'deps/mjc-build/release/twelve-dollar-benchmark'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--users', type=int, default=275000)
p.add_argument('--duration', default='5m')
p.add_argument('--ramp-up', default='60s')
p.add_argument('--warmup', action='store_true')
p.add_argument('--score', action='store_true')
p.add_argument('--start-users', type=int, default=275000)
p.add_argument('--max-users', type=int, default=500000)
p.add_argument('--label', default='mjc')
p.add_argument('--server-arg', action='append', default=[])
p.add_argument('--inspect-rings', action='store_true', help='read-only diagnostic sidecar; not a clean qualification')
p.add_argument('--pause-after', type=float, help='diagnostic only: pause server after this many benchmark seconds')
p.add_argument('--pause-seconds', type=float, default=0.5)
a = p.parse_args()
if not GENERATOR.exists() or not Path('/nix/store').exists():
    p.error('build the Ubuntu generator first; this local runner requires Nix for the server runtime')
revision = subprocess.check_output(['git', '-C', str(ROOT/'deps/twelve-dollar-benchmark'),
                                    'rev-parse', 'HEAD'], text=True).strip()
if revision != REVISION:
    p.error('generator checkout does not match the pinned revision')
out = ROOT/'results'/(a.label+'-'+time.strftime('%Y%m%dT%H%M%S'))
out.mkdir(parents=True, exist_ok=False)
server = 'nasm-mjc-'+uuid.uuid4().hex[:10]
clients = []
inspector = None
mounts = ['-v', f'{REPO}:/repo:ro', '-v', f'{out}:/results',
          '-v', '/nix/store:/nix/store:ro']
image = 'ubuntu:24.04'
binary = '/repo/'+str(GENERATOR.relative_to(REPO))
evidence = dict(generator_revision=revision,
    generator_sha256=hashlib.sha256(GENERATOR.read_bytes()).hexdigest(),
    server_sha256=hashlib.sha256((ROOT/'bin/server').read_bytes()).hexdigest(),
    arguments=vars(a), server_limits=dict(cpus=1, cpuset='0', memory_bytes=2**31,
        swap_bytes=0, nofile=65535),
    client_limits=dict(cpuset='1-7,9-15', memory_bytes=12*2**30, swap_bytes=0,
        nofile=1048576, tokio_threads=4),
    topology='separate cgroups; private shared loopback and PID namespace; host cgroup namespace read-only in client',
    samples=[], runs=[])

def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout

def state(name):
    return json.loads(run(['docker', 'inspect', name]))[0]['State']

def save():
    (out/'evidence.json').write_text(json.dumps(evidence, indent=2)+'\n')

def sample():
    raw = run(['docker', 'exec', server, 'sh', '-c',
        'cat /sys/fs/cgroup/memory.current /sys/fs/cgroup/memory.peak '
        '/sys/fs/cgroup/memory.events /sys/fs/cgroup/cpu.stat; '
        'stat -c "wal_bytes %s" /work/feed.db-wal 2>/dev/null; '
        'cat /proc/net/snmp'])
    lines = raw.splitlines()
    row = dict(time=time.time(), memory_current=int(lines[0]), memory_peak=int(lines[1]))
    for line in lines[2:]:
        fields = line.split()
        if len(fields) == 2 and not fields[0].endswith(':'):
            row[fields[0]] = int(fields[1])
    tcp = [line.split()[1:] for line in lines if line.startswith('Tcp:')]
    if len(tcp) == 2:
        row['namespace_established'] = int(dict(zip(*tcp))['CurrEstab'])
    evidence['samples'].append(row)
    return row

def workload(label, options):
    client = server+'-'+label
    clients.append(client)
    command = ['docker', 'run', '--name', client, '--network', 'container:'+server,
        '--pid', 'container:'+server, '--cgroupns', 'host',
        '-v', '/sys/fs/cgroup:/sys/fs/cgroup:ro',
        '--cpuset-cpus', '1-7,9-15', '--memory', '12g', '--memory-swap', '12g',
        '--ulimit', 'nofile=1048576:1048576', '-e', 'TOKIO_WORKER_THREADS=4',
        '--entrypoint', binary, *mounts, image, '--orchestrate', '127.0.0.1', '8080',
        '--source-ips', '127.0.0.[2-65]', '--tokens', '/repo/seed/tokens.json',
        '--application-pids', '1', '--sample-interval', '5',
        '--label', 'NASM io_uring', '--output-dir', '/results/'+label, *options]
    print(f'{label}: {options}', flush=True)
    evidence['active_command'] = command
    save()
    with (out/(label+'.log')).open('w') as log:
        proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        started = time.monotonic()
        paused_once = False
        while proc.poll() is None:
            time.sleep(10)
            if (label == 'benchmark' and a.pause_after is not None and not paused_once
                    and time.monotonic()-started >= a.pause_after):
                # A diagnostic fault injection, never a qualification run.
                # Resume in finally even if the pause wait is interrupted.
                run(['docker', 'pause', server])
                evidence['injected_pause'] = dict(start=time.time(), requested_seconds=a.pause_seconds)
                try:
                    time.sleep(a.pause_seconds)
                finally:
                    run(['docker', 'unpause', server])
                    evidence['injected_pause']['finished'] = time.time()
                paused_once = True
            try:
                row = sample()
            except subprocess.CalledProcessError:
                evidence['server_failure_state'] = state(server)
                subprocess.run(['docker', 'stop', '--time', '20', client], capture_output=True)
                break
            print(f'  memory={row["memory_current"]/2**20:.1f} MiB '
                  f'peak={row["memory_peak"]/2**20:.1f} MiB '
                  f'WAL={row.get("wal_bytes",0)/2**20:.1f} MiB '
                  f'namespace TCP={row.get("namespace_established")}', flush=True)
            workflow = out/label/'workflow.json'
            if workflow.exists():
                try:
                    progress = json.loads(workflow.read_text())
                    print(f'  search current={progress.get("current")} '
                          f'trials={len(progress.get("trials",[]))}', flush=True)
                except (ValueError, OSError):
                    pass
            save()
        proc.wait(timeout=30)
    result = dict(name=label, exit_code=proc.returncode,
                  client_state=state(client), server_state=state(server))
    for filename in ('aggregate.json', 'workflow.json'):
        path = out/label/filename
        if path.exists():
            result[filename] = json.loads(path.read_text())
    evidence['runs'].append(result)
    save()
    print(f'{label}: exit={proc.returncode}; reports: {out/label}', flush=True)
    report = result.get('aggregate.json')
    # Endpoint checks are diagnostic in the upstream benchmark. Require them
    # too for our qualification; a successful HTTP status alone is insufficient.
    checks_ok = (report is not None and report.get('passed') is True and
                 report.get('aggregate', {}).get('checks_failed') == 0)
    if a.score and report is None:
        # Search intentionally includes failing population trials. The native
        # exit status decides whether a confirmation passed, but do not hide
        # failed endpoint checks anywhere in its emitted trial reports.
        reports = list((out/label).rglob('aggregate.json'))
        checks_ok = bool(reports) and all(
            json.loads(path.read_text()).get('aggregate', {}).get('checks_failed') == 0
            for path in reports)
    return (proc.returncode == 0 and checks_ok and
            not result['server_state']['OOMKilled'] and
            not result['client_state']['OOMKilled'])

try:
    run(['docker', 'run', '-d', '--name', server, '--network', 'none',
        '--cpus', '1', '--cpuset-cpus', '0', '--memory', '2g', '--memory-swap', '2g',
        '--ulimit', 'nofile=65535:65535', '--security-opt', 'seccomp=unconfined',
        '--entrypoint', 'sh', *mounts, '-e', 'SQLITE_PATH=/work/feed.db',
        '-e', 'JWT_SECRET=twelve-dollar-challenge', '-e', 'HOST=127.0.0.1',
        '-e', 'PORT=8080', image, '-c',
        'mkdir -p /work && cp /repo/seed/feed.db /work/feed.db && '
        'exec /repo/submissions/x86_64-uring-imlunahey/bin/server '+shlex.join(a.server_arg)])
    for _ in range(100):
        health = subprocess.run(['docker', 'exec', server, sys.executable, '-c',
            'import urllib.request; print(urllib.request.urlopen("http://127.0.0.1:8080/health").read())'],
            capture_output=True)
        if health.returncode == 0 and b'"db":"ok"' in health.stdout:
            break
        time.sleep(.1)
    else:
        raise RuntimeError('server did not become healthy')
    evidence['server_inspect'] = json.loads(run(['docker', 'inspect', server]))[0]
    if a.inspect_rings:
        symbols = {}
        wanted = {'ring_contexts', 'uring_count', 'request_count', 'active_count',
                  'input_allocated', 'output_allocated', 'wal_frames',
                  'wal_log', 'wal_checkpointed', 'checkpoint_requested'}
        for line in run(['nm', '-n', str(ROOT/'bin/server')]).splitlines():
            fields = line.split()
            if len(fields) == 3 and fields[2] in wanted:
                symbols[fields[2]] = int(fields[0], 16)
        if symbols.keys() != wanted:
            raise RuntimeError('missing symbols for read-only ring diagnostics')
        inspector = server+'-inspector'
        run(['docker', 'run', '-d', '--name', inspector, '--network', 'none',
             '--pid', 'container:'+server, '--cap-add', 'SYS_PTRACE',
             '--cpuset-cpus', '7', '--memory', '128m', '--memory-swap', '128m',
             '--entrypoint', sys.executable, *mounts, image,
             '/repo/submissions/x86_64-uring-imlunahey/tools/inspect-rings.py',
             '--symbols', json.dumps(symbols), '--output', '/results/rings.jsonl'])
    if a.warmup and not a.score:
        if not workload('warmup', ['--users', '1000', '--ramp-up', '1s', '--duration', '2m']):
            raise RuntimeError('warmup failed; inspect reports')
    options = (['--score', '--start-users', str(a.start_users), '--max-users', str(a.max_users)]
               if a.score else ['--users', str(a.users), '--duration', a.duration])
    options += ['--ramp-up', a.ramp_up]
    passed = workload('benchmark', options)
    if state(server)['Running']:
        sample()
        run(['docker', 'stop', '--time', '20', server])
    evidence['server_final_state'] = state(server)
    passed &= (evidence['server_final_state']['ExitCode'] == 0 and
               not any(s.get('oom_kill', 0) for s in evidence['samples']))
    if passed:
        run(['docker', 'cp', server+':/work/feed.db', str(out/'feed.db')])
        import sqlite3
        with sqlite3.connect(out/'feed.db') as db:
            evidence['integrity_check'] = db.execute('PRAGMA integrity_check').fetchone()[0]
        passed &= evidence['integrity_check'] == 'ok'
        (out/'feed.db').unlink()
    evidence['passed'] = passed
finally:
    save()
    logs = subprocess.run(['docker', 'logs', server], capture_output=True, text=True)
    (out/'server.log').write_text(logs.stdout+logs.stderr)
    for container in [*clients, *([inspector] if inspector else []), server]:
        subprocess.run(['docker', 'rm', '-f', container], capture_output=True)
    print(f'Evidence: {out}', flush=True)
if not evidence.get('passed'):
    raise SystemExit(1)
