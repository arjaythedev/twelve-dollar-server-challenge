#!/usr/bin/env python3
"""Run the unmodified official workload against a constrained local container."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import urllib.request
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--vus', type=int, required=True)
parser.add_argument('--hold', default='5m')
parser.add_argument('--ramp', default='60s')
parser.add_argument('--warmup', default='2m', help='0 to skip, for screening only')
parser.add_argument('--port', type=int, default=3100)
parser.add_argument('--image', default='twelve-fastify-qualification:local')
parser.add_argument('--label', required=True)
args = parser.parse_args()
if not args.label.replace('-', '').replace('_', '').isalnum():
    parser.error('label must contain only letters, numbers, hyphens and underscores')
here = Path(__file__).resolve().parent
root = here.parents[1]
out = here / 'results'
out.mkdir(exist_ok=True)
name = f'fastify-benchmark-{uuid.uuid4().hex[:10]}'

def command(argv, **kwargs):
    return subprocess.run(argv, check=True, text=True, **kwargs)

def load(label, vus, hold, ramp):
    with (out / f'{label}.log').open('w') as log:
        result = subprocess.run([
            'docker', 'run', '--rm', '--name', f'{name}-load',
            '--user', f'{os.getuid()}:{os.getgid()}', '--network=host',
            '--ulimit', 'nofile=1048576:1048576', '--memory=48g', '--memory-swap=48g',
            '-v', f'{root}:/work:ro', '-v', f'{out}:/out',
            '-e', f'BASE_URL=http://127.0.0.1:{args.port}', '-e', f'VUS={vus}',
            '-e', f'DURATION={hold}', '-e', f'RAMP_UP={ramp}',
            'grafana/k6:1.4.2', 'run', '--quiet', '--local-ips=127.0.0.2-127.0.0.17',
            f'--summary-export=/out/{label}.json', '/work/bench/load.js'
        ], stdout=log, stderr=subprocess.STDOUT)
    if result.returncode not in (0, 99):
        raise RuntimeError(f'load generator failed ({result.returncode}); see {label}.log')
    summary = json.loads((out / f'{label}.json').read_text())
    metrics = summary['metrics']
    result = {
        'exit_code': result.returncode,
        'duration_ms': metrics['http_req_duration'],
        'failures': metrics['http_req_failed'],
        'checks': metrics['checks'],
        'requests': metrics['http_reqs'],
    }
    print(json.dumps({'run': label, **result}), flush=True)
    return result

try:
    command([
        'docker', 'run', '-d', '--name', name, '--network=host', '--cpus=1',
        '--memory=2g', '--memory-swap=2g', '--ulimit', 'nofile=65535:65535',
        '-e', 'JWT_SECRET=twelve-dollar-challenge', '-e', 'HOST=0.0.0.0',
        '-e', f'PORT={args.port}', '-e', 'SQLITE_PATH=/tmp/feed.db',
        '-v', f'{root / "seed"}:/seed:ro', args.image,
        'bash', '-c', 'cp /seed/feed.db /tmp/feed.db; exec bash start.sh'
    ], stdout=subprocess.DEVNULL)
    for attempt in range(600):
        try:
            with urllib.request.urlopen(f'http://127.0.0.1:{args.port}/health', timeout=1) as response:
                if response.status == 200:
                    break
        except OSError:
            time.sleep(.1)
    else:
        raise RuntimeError('server failed to become healthy')
    evidence = {
        'vus': args.vus, 'hold': args.hold, 'ramp': args.ramp, 'warmup': args.warmup,
        'image': command(['docker', 'inspect', '--format', '{{.Image}}', name], capture_output=True).stdout.strip(),
        'source_sha256': hashlib.sha256((here / 'server.ts').read_bytes()).hexdigest(),
        'workload_sha256': hashlib.sha256((root / 'bench/load.js').read_bytes()).hexdigest(),
        'server_cpu_quota': 1, 'server_memory_bytes': 2147483648, 'server_swap_bytes': 0,
        'nofile': 65535, 'network': 'host', 'generator': 'grafana/k6:1.4.2',
        'source_addresses': '127.0.0.2-127.0.0.17',
    }
    if args.warmup != '0':
        evidence['warmup_result'] = load(f'{args.label}-warmup', 1000, args.warmup, '1s')
        if evidence['warmup_result']['exit_code'] != 0:
            raise RuntimeError('warm-up failed; scored run was not started')
    evidence['result'] = load(args.label, args.vus, args.hold, args.ramp)
    evidence['memory_peak_bytes'] = int(command([
        'docker', 'exec', name, 'cat', '/sys/fs/cgroup/memory.peak'
    ], capture_output=True).stdout)
    evidence['oom_killed'] = command([
        'docker', 'inspect', '--format', '{{.State.OOMKilled}}', name
    ], capture_output=True).stdout.strip() == 'true'
    (out / f'{args.label}-evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(f"Peak cgroup memory: {evidence['memory_peak_bytes'] / 1048576:.1f} MiB", flush=True)
    raise SystemExit(evidence['result']['exit_code'])
finally:
    subprocess.run(['docker', 'rm', '-f', f'{name}-load'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(['docker', 'rm', '-f', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
