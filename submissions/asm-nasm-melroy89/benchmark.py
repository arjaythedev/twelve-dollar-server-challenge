#!/usr/bin/env python3
"""Unchanged official workload in a constrained local container.
Adapted from this repository's typescript-fastify-melroy89 benchmark harness (MIT).
Local same-host diagnostics do not establish an organizer droplet score.
"""
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
parser.add_argument('--image', default='twelve-nasm-qualification:local')
parser.add_argument('--start-script', action='store_true', help='run the submission start.sh unchanged (contender qualification)')
parser.add_argument('--binary-path', default='/app/bin/server', help='actual application executable to checksum')
parser.add_argument('--probe-image', help='external curl image for private-network health checks when the contender lacks curl')
parser.add_argument('--label', required=True)
parser.add_argument('--server-arg', action='append', default=[])
parser.add_argument('--generator-memory', default='48g')
parser.add_argument('--generator-procs', type=int, help='optional k6 Go runtime parallelism; not server affinity')
parser.add_argument('--generator-gc-trace', action='store_true', help='diagnostic Go GC timings in generator log')
parser.add_argument('--generator-gogc', type=int, help='optional GOGC setting for controlled k6 comparisons')
parser.add_argument('--generator-memory-limit', help='optional GOMEMLIMIT value, for example 44GiB')
parser.add_argument('--isolated-network', action='store_true',
                    help='share a private loopback namespace, avoiding host conntrack exhaustion')
args = parser.parse_args()
if args.generator_procs is not None and args.generator_procs < 1:
    parser.error('generator-procs must be positive')
if args.start_script and args.server_arg:
    parser.error('unchanged start-script mode cannot take experimental server arguments')
if not args.label.replace('-', '').replace('_', '').isalnum():
    parser.error('label must contain only letters, numbers, hyphens and underscores')
here = Path(__file__).resolve().parent
root = here.parents[1]
out = here / 'results'
out.mkdir(exist_ok=True)
name = f'nasm-benchmark-{uuid.uuid4().hex[:10]}'
server_cgroup = None
server_samples = []

def command(argv, **kwargs):
    return subprocess.run(argv, check=True, text=True, **kwargs)

def load(label, vus, hold, ramp):
    samples = []
    generator_state = {}
    cgroup = None
    with (out / f'{label}.log').open('w') as log:
        process = subprocess.Popen([
            'docker', 'run', '--name', f'{name}-load',
            '--user', f'{os.getuid()}:{os.getgid()}',
            '--network=' + (f'container:{name}' if args.isolated_network else 'host'),
            '--ulimit', 'nofile=1048576:1048576', f'--memory={args.generator_memory}', f'--memory-swap={args.generator_memory}',
            '-v', f'{root}:/work:ro', '-v', f'{out}:/out',
            '-e', f'BASE_URL=http://127.0.0.1:{args.port}', '-e', f'VUS={vus}',
            '-e', f'DURATION={hold}', '-e', f'RAMP_UP={ramp}',
            *(['-e', f'GOMAXPROCS={args.generator_procs}'] if args.generator_procs is not None else []),
            *(['-e', 'GODEBUG=gctrace=1'] if args.generator_gc_trace else []),
            *(['-e', f'GOGC={args.generator_gogc}'] if args.generator_gogc is not None else []),
            *(['-e', f'GOMEMLIMIT={args.generator_memory_limit}'] if args.generator_memory_limit else []),
            'grafana/k6:1.4.2', 'run', '--quiet', '--local-ips=127.0.0.2-127.0.0.17',
            f'--summary-export=/out/{label}.json', '/work/bench/load.js'
        ], stdout=log, stderr=subprocess.STDOUT)
        try:
            while process.poll() is None:
                try:
                    if cgroup is None:
                        probe = subprocess.run(['docker', 'inspect', '--format', '{{.State.Pid}}', f'{name}-load'],
                                               text=True, capture_output=True)
                        if probe.returncode == 0 and int(probe.stdout) > 0:
                            membership = Path(f'/proc/{int(probe.stdout)}/cgroup').read_text()
                            relative = next(line[3:] for line in membership.splitlines() if line.startswith('0::'))
                            cgroup = Path('/sys/fs/cgroup') / relative.lstrip('/')
                    if cgroup is not None:
                        cpu = dict(line.split() for line in (cgroup / 'cpu.stat').read_text().splitlines())
                        samples.append({'monotonic_seconds': time.monotonic(),
                                        'cpu_usage_usec': int(cpu['usage_usec']),
                                        'memory_current_bytes': int((cgroup / 'memory.current').read_text()),
                                        'memory_peak_bytes': int((cgroup / 'memory.peak').read_text())})
                except (OSError, ValueError, StopIteration):
                    pass
                if server_cgroup is not None:
                    try:
                        stats = dict(line.split() for line in (server_cgroup / 'cpu.stat').read_text().splitlines())
                        server_samples.append(dict(monotonic_seconds=time.monotonic(), cpu_stat=stats,
                            memory_peak_bytes=int((server_cgroup / 'memory.peak').read_text())))
                    except OSError:
                        pass
                time.sleep(1)
            inspected = subprocess.run(['docker', 'inspect', '--format', '{{json .State}}', f'{name}-load'],
                                       text=True, capture_output=True)
            if inspected.returncode == 0:
                generator_state = json.loads(inspected.stdout)
        finally:
            if process.poll() is None:
                subprocess.run(['docker', 'rm', '-f', f'{name}-load'], capture_output=True)
                process.wait(timeout=30)
            resources = {'samples': samples, 'state': generator_state, 'exit_code': process.returncode,
                         'memory_limit': args.generator_memory, 'generator': 'grafana/k6:1.4.2',
                         'gomaxprocs': args.generator_procs, 'gc_trace': args.generator_gc_trace,
                         'gogc': args.generator_gogc, 'gomemlimit': args.generator_memory_limit,
                         'boundary': 'Same-host generator outside server quota; sampled resources are diagnostic.'}
            (out / f'{label}-generator.json').write_text(json.dumps(resources, indent=2) + '\n')
            subprocess.run(['docker', 'rm', '-f', f'{name}-load'], capture_output=True)
    if process.returncode not in (0, 99):
        raise RuntimeError(f'load generator failed ({process.returncode}); see {label}.log and generator resource record')
    summary = json.loads((out / f'{label}.json').read_text())
    metrics = summary['metrics']
    result = {
        'exit_code': process.returncode,
        'duration_ms': metrics['http_req_duration'],
        'failures': metrics['http_req_failed'],
        'checks': metrics['checks'],
        'requests': metrics['http_reqs'],
    }
    print(json.dumps({'run': label, **result}), flush=True)
    # Every official workload request has one status check. Transport failures
    # can legitimately miss that check within the unchanged <1% failure limit.
    missed = metrics['checks'].get('fails', 1)
    http_failed = round(metrics['http_req_failed']['value'] * metrics['http_reqs']['count'])
    result['all_status_checks_pass'] = missed == 0
    result['unexplained_status_check_failures'] = max(0, missed - http_failed)
    result['json_semantics'] = 'not established by status-only k6 checks; validate separately'
    result['accepted'] = result['exit_code'] == 0 and result['unexplained_status_check_failures'] == 0
    result['strict_zero_failure_pass'] = result['accepted'] and missed == 0 and http_failed == 0
    return result

try:
    startup_started = time.monotonic()
    startup_deadline = startup_started + 60
    command([
        'docker', 'run', '-d', '--name', name,
        '--network=' + ('none' if args.isolated_network else 'host'), '--cpus=1',
        '--memory=2g', '--memory-swap=2g', '--ulimit', 'nofile=65535:65535',
        '--security-opt', 'seccomp=unconfined',
        '-e', 'JWT_SECRET=twelve-dollar-challenge', '-e', 'HOST=0.0.0.0',
        '-e', f'PORT={args.port}', '-e', 'SQLITE_PATH=/tmp/feed.db',
        '-v', f'{root / "seed"}:/seed:ro', args.image,
        'bash', '-c', 'cp /seed/feed.db /tmp/feed.db && ' +
        ('exec bash start.sh' if args.start_script else 'exec bin/server "$@"'), 'submission', *args.server_arg
    ], stdout=subprocess.DEVNULL)
    while time.monotonic() < startup_deadline:
        try:
            if args.isolated_network:
                prefix = (['docker', 'run', '--rm', f'--network=container:{name}',
                           '--entrypoint', 'curl', args.probe_image] if args.probe_image else
                          ['docker', 'exec', name, 'curl'])
                probe = subprocess.run([*prefix, '-fsS', '--max-time', '1',
                                        f'http://127.0.0.1:{args.port}/health'],
                                       capture_output=True, text=True)
                if probe.returncode == 0 and json.loads(probe.stdout).get('db') == 'ok':
                    break
                time.sleep(.1)
                continue
            with urllib.request.urlopen(f'http://127.0.0.1:{args.port}/health', timeout=1) as response:
                if response.status == 200:
                    break
        except OSError:
            time.sleep(.1)
    else:
        raise RuntimeError('server failed to become healthy')
    healthy_seconds = time.monotonic() - startup_started
    if healthy_seconds > 60:
        raise RuntimeError('server exceeded the 60-second startup gate')
    build_identity = json.loads(command(['docker', 'exec', name, 'cat', '/app/build/identity.json'], capture_output=True).stdout)
    workspace_source_sha256 = {
        str(p.relative_to(here)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted((here / 'src').glob('*'))
    }
    evidence = {
        'vus': args.vus, 'hold': args.hold, 'ramp': args.ramp, 'warmup': args.warmup,
        'image': command(['docker', 'inspect', '--format', '{{.Image}}', name], capture_output=True).stdout.strip(),
        'source_sha256': build_identity.get('source_sha256', workspace_source_sha256),
        'workspace_source_sha256': workspace_source_sha256,
        'build_identity': build_identity,
        'runtime_binary_sha256': command(['docker', 'exec', name, 'sha256sum', args.binary_path], capture_output=True).stdout.split()[0],
        'runtime_binary_path': args.binary_path, 'start_script_mode': args.start_script,
        'healthy_seconds': healthy_seconds, 'probe_image': args.probe_image,
        'host_kernel': os.uname().release, 'server_args': args.server_arg,
        'host_cpu_model': next(line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name')),
        'host_logical_cpus': os.cpu_count(),
        'local_seccomp_exception': 'unconfined: default Docker seccomp blocks io_uring',
        'workload_sha256': hashlib.sha256((root / 'bench/load.js').read_bytes()).hexdigest(),
        'server_cpu_quota': 1, 'server_memory_bytes': 2147483648, 'server_swap_bytes': 0,
        'nofile': 65535, 'network': 'private-shared-loopback' if args.isolated_network else 'host',
        'generator': 'grafana/k6:1.4.2', 'generator_procs': args.generator_procs, 'generator_gc_trace': args.generator_gc_trace,
        'generator_gogc': args.generator_gogc, 'generator_memory_limit': args.generator_memory_limit,
        'source_addresses': '127.0.0.2-127.0.0.17',
    }
    evidence['source_sha256'] = evidence['build_identity'].get('source_sha256', evidence['workspace_source_sha256'])
    server_pid = int(command(['docker','inspect','--format','{{.State.Pid}}',name],capture_output=True).stdout)
    membership = Path(f'/proc/{server_pid}/cgroup').read_text()
    relative = next(line[3:] for line in membership.splitlines() if line.startswith('0::'))
    server_cgroup = Path('/sys/fs/cgroup') / relative.lstrip('/')
    cpu_start = command(['docker', 'exec', name, 'cat', '/sys/fs/cgroup/cpu.stat'], capture_output=True).stdout
    measure_start = time.monotonic()
    if args.warmup != '0':
        evidence['warmup_result'] = load(f'{args.label}-warmup', 1000, args.warmup, '1s')
        if not evidence['warmup_result']['accepted']:
            raise RuntimeError('warm-up failed; scored run was not started')
    evidence['result'] = load(args.label, args.vus, args.hold, args.ramp)
    evidence['elapsed_seconds_including_warmup'] = time.monotonic() - measure_start
    evidence['cpu_stat_start'] = cpu_start
    state = json.loads(command(['docker','inspect','--format','{{json .State}}',name],capture_output=True).stdout)
    evidence['server_state'] = state
    evidence['server_samples'] = server_samples
    def server_read(path):
        read = subprocess.run(['docker','exec',name,'cat',path],capture_output=True,text=True)
        return read.stdout if read.returncode == 0 else None
    evidence['cpu_stat_end'] = server_read('/sys/fs/cgroup/cpu.stat')
    if evidence['cpu_stat_end'] is None and server_samples:
        evidence['cpu_stat_end_sampled'] = server_samples[-1]['cpu_stat']
    evidence['memory_stat_end'] = server_read('/sys/fs/cgroup/memory.stat')
    logs = subprocess.run(['docker','logs',name],capture_output=True,text=True)
    evidence['server_log'] = logs.stdout + logs.stderr
    peak = server_read('/sys/fs/cgroup/memory.peak')
    evidence['memory_peak_bytes'] = int(peak) if peak is not None else max((x['memory_peak_bytes'] for x in server_samples),default=None)
    evidence['memory_peak_sampled'] = peak is None
    evidence['oom_killed'] = state['OOMKilled']
    if not state['Running'] or state['OOMKilled']:
        evidence['result']['accepted'] = False
        evidence['result']['server_terminated'] = True
    (out / f'{args.label}-evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print("Peak cgroup memory:", evidence["memory_peak_bytes"], "bytes; sampled:", evidence["memory_peak_sampled"], flush=True)
    raise SystemExit(0 if evidence['result']['accepted'] else 99)
finally:
    subprocess.run(['docker', 'rm', '-f', f'{name}-load'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(['docker', 'rm', '-f', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
