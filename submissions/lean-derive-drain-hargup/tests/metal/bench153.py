#!/usr/bin/env python3
"""Same-host load harness for the #153 improvement study (runs on the benchmark host as root,
inside `unshare -n`). Derived from the leader study's tests/metal/bench.py (branch
lean-lowmem-leader-study-hargup); additions: kernel memory accounting outside the server's
cgroup (/proc/meminfo, /proc/slabinfo, /proc/net/sockstat) sampled through the run, an
optional stall injected by freezing the server's cgroup, and the server's STATS lines.

Server: systemd scope on CPU 0, MemoryMax=2G, no swap, LimitNOFILE=65535, UID 65534, fresh
seed copy, HOST=127.0.0.1:3000 in a private network namespace. Load: the native generator
(/root/twelve/loadgen, bench/load.js user model) on CPUs 1-5,7-11 from 40 loopback source IPs.
"""
import argparse, datetime, json, os, pathlib, re, resource, shutil, subprocess, time, threading, tempfile

ROOT = pathlib.Path(os.environ.get('STUDY_ROOT', '/root/twelve/lean-153-improve'))
CHALLENGE = pathlib.Path('/root/twelve/challenge')


def readstat(path):
    return {k: int(v) for k, v in (line.split() for line in path.read_text().splitlines())}


def meminfo():
    out = {}
    for line in pathlib.Path('/proc/meminfo').read_text().splitlines():
        k, v = line.split(':', 1)
        out[k] = int(v.split()[0])          # kB
    return out


def slabinfo():
    out = {}
    for line in pathlib.Path('/proc/slabinfo').read_text().splitlines()[2:]:
        f = line.split()
        out[f[0]] = (int(f[1]), int(f[3]))  # active objects, object size
    return out


def sockstat():
    out = {}
    for line in pathlib.Path('/proc/net/sockstat').read_text().splitlines():
        k, rest = line.split(':', 1)
        f = rest.split()
        out[k] = {f[i]: int(f[i + 1]) for i in range(0, len(f) - 1, 2)}
    return out


def slab_delta(a, b, conns):
    rows = []
    for k, (n, sz) in b.items():
        d = n - a.get(k, (0, sz))[0]
        if abs(d * sz) > (1 << 20):
            rows.append({'cache': k, 'objects': d, 'size': sz, 'mb': round(d * sz / 2**20, 1),
                         'bytes_per_conn': round(d * sz / conns, 1) if conns else None})
    rows.sort(key=lambda r: -abs(r['mb']))
    return rows


def load_details(d, samples, host, started):
    cfg = d['config']
    whole = d['windows']['whole']['endpoints']['all']
    hold = d['windows']['hold']['endpoints']['all']
    tl = d['timeline']
    ramp = [t for t in tl if t['t'] <= cfg['ramp_s'] + 5]
    hold_tl = [t for t in tl if cfg['ramp_s'] < t['t'] <= cfg['ramp_s'] + cfg['duration_s']]
    full = [t['t'] for t in tl if t['conns'] >= cfg['vus']]
    r = dict(verdict=d['verdict'], whole_p95_ms=whole['total_ms']['p95'], whole_p99_ms=whole['total_ms']['p99'],
             whole_max_ms=whole['total_ms']['max'], whole_error_rate=whole['error_rate'],
             whole_errors_by_kind=whole['errors_by_kind'], hold_errors_by_kind=hold['errors_by_kind'],
             hold_requests=hold['count'], hold_p95_ms=hold['total_ms']['p95'], hold_p99_ms=hold['total_ms']['p99'],
             hold_p999_ms=hold['total_ms']['p999'], hold_max_ms=hold['total_ms']['max'],
             counters=d['counters'], generator=d.get('generator'),
             ramp_max_connecting=max((t['connecting'] for t in ramp), default=None),
             ramp_worst_second_p99_ms=max((t['p99_ms'] for t in ramp), default=None),
             seconds_to_all_connected=full[0] if full else None,
             max_conns=max((t['conns'] for t in tl), default=None),
             min_hold_conns=min((t['conns'] for t in hold_tl if t['t'] > cfg['ramp_s'] + 6), default=None),
             hold_worst_second_p99_ms=max((t['p99_ms'] for t in hold_tl), default=None),
             hold_mean_rps=sum(t['reqs'] for t in hold_tl) / max(1, len(hold_tl)))
    t0, t1 = started + d['windows']['hold']['start_s'], started + d['windows']['hold']['end_s']
    hs = [h for h in host if t0 <= h[0] <= t1 and h[2] is not None]
    ss = [s for s in samples if t0 <= s[0] <= t1]
    if len(hs) > 1 and len(ss) > 1:
        span = hs[-1][0] - hs[0][0]
        host_cores = (hs[-1][1] - hs[0][1]) / 1e6 / span
        client_cores = (hs[-1][2] - hs[0][2]) / 1e6 / span
        server_cores = (ss[-1][1] - ss[0][1]) / 1e6 / (ss[-1][0] - ss[0][0])
        r.update(hold_server_cores=server_cores, hold_client_cores=client_cores, hold_host_busy_cores=host_cores,
                 hold_other_host_cores=host_cores - server_cores - client_cores)
    return r


def main():
    p = argparse.ArgumentParser()
    p.add_argument('variant')
    p.add_argument('--vus', type=int, default=300000)
    p.add_argument('--ramp', type=int, default=60)
    p.add_argument('--duration', type=int, default=90)
    p.add_argument('--rampdown', type=int, default=30)
    p.add_argument('--cpu-quota', help='systemd CPUQuota of the server scope, e.g. 50%%')
    p.add_argument('--quota-period', default='10ms')
    p.add_argument('--stall-at', type=float, help='freeze the server cgroup this many seconds after load start')
    p.add_argument('--stall-for', type=float, default=2.0)
    p.add_argument('--snap-at', type=float, help='kernel memory snapshot time (default: middle of the hold)')
    p.add_argument('--perf-at', type=int)
    p.add_argument('--perf-for', type=int, default=30)
    p.add_argument('--syscalls-at', type=int, help='count the server\'s system calls for 20 s from this time (perf stat)')
    p.add_argument('--hw-at', type=int, help='hardware counters only (cycles, instructions; no tracepoints) for --hw-for s')
    p.add_argument('--hw-for', type=int, default=60)
    p.add_argument('--tag', default='')
    a = p.parse_args()
    label = f'{a.variant}{"-" + a.tag if a.tag else ""}-{time.time_ns()}'
    out = ROOT / 'results' / label
    out.mkdir(parents=True)
    subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=True)
    unit = 'l153-' + re.sub(r'[^A-Za-z0-9_.-]', '_', label)   # systemd escapes '+' etc. in cgroup names
    cg = pathlib.Path('/sys/fs/cgroup/system.slice') / (unit + '.scope')
    work = pathlib.Path(tempfile.mkdtemp(prefix='l153-'))   # /root is not traversable by UID 65534
    os.chown(work, 65534, 65534)
    db_path = work / 'feed.db'
    shutil.copyfile(CHALLENGE / 'seed/feed.db', db_path)
    os.chown(db_path, 65534, 65534)
    binary = work / 'server'
    shutil.copyfile(ROOT / 'artifacts' / a.variant / 'server', binary)
    binary.chmod(0o755)
    cmd = ['systemd-run', '--quiet', '--scope', '--unit=' + unit,
           '-p', 'AllowedCPUs=0', '-p', 'MemoryMax=2G', '-p', 'MemorySwapMax=0'] + \
          (['-p', 'CPUQuota=' + a.cpu_quota, '-p', 'CPUQuotaPeriodSec=' + a.quota_period] if a.cpu_quota else []) + \
          ['prlimit', '--nofile=65535:65535', 'setpriv', '--reuid=65534', '--regid=65534', '--clear-groups',
           'env', 'SQLITE_PATH=' + str(db_path), 'JWT_SECRET=twelve-dollar-challenge', 'HOST=127.0.0.1', 'PORT=3000',
           str(binary)]
    proc = subprocess.Popen(cmd, stdout=(out / 'server.log').open('w'), stderr=subprocess.STDOUT)
    result = {'variant': a.variant, 'tag': a.tag, 'directory': str(out), 'vus': a.vus, 'ramp': a.ramp,
              'duration': a.duration, 'rampdown': a.rampdown, 'cpu_quota': a.cpu_quota,
              'quota_period': a.quota_period if a.cpu_quota else None,
              'stall_at': a.stall_at, 'stall_for': a.stall_for if a.stall_at is not None else None,
              'binary_sha256': subprocess.run(['sha256sum', str(binary)], capture_output=True, text=True).stdout.split()[0],
              'kernel': os.uname().release, 'started_utc': datetime.datetime.utcnow().isoformat() + 'Z'}
    try:
        t = time.monotonic()
        while time.monotonic() - t < 60:
            if subprocess.run(['curl', '-sf', '--max-time', '1', 'http://127.0.0.1:3000/health'],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
                break
            if proc.poll() is not None:
                raise RuntimeError((out / 'server.log').read_text())
            time.sleep(.05)
        else:
            raise RuntimeError('startup timeout')
        result['startup_s'] = time.monotonic() - t
        base = {'meminfo': meminfo(), 'slab': slabinfo(), 'sockstat': sockstat(),
                'cg_current': int((cg / 'memory.current').read_text())}
        u0 = readstat(cg / 'cpu.stat')['usage_usec']
        resource.setrlimit(resource.RLIMIT_NOFILE, (1048576, 1048576))
        lg = ['/root/twelve/loadgen/loadgen', '--url', 'http://127.0.0.1:3000',
              '--tokens', str(CHALLENGE / 'seed/tokens.json'), '--threads', '8',
              '--cpus', '1-5,7-11', '--out', str(out / 'load.json'),
              '--vus', str(a.vus), '--ramp', str(a.ramp), '--duration', str(a.duration),
              '--rampdown', str(a.rampdown), '--src', '127.0.1.1-127.0.1.40']
        samples, host, mem = [], [], []
        done = threading.Event()
        hz = os.sysconf('SC_CLK_TCK')
        client = None

        def host_busy_usec():
            f = [int(x) for x in pathlib.Path('/proc/stat').read_text().split('\n', 1)[0].split()[1:]]
            return (sum(f[:8]) - f[3] - f[4]) * 1_000_000 // hz

        def client_usec():
            try:
                f = pathlib.Path(f'/proc/{client.pid}/stat').read_text().rsplit(')', 1)[1].split()
                return (int(f[11]) + int(f[12])) * 1_000_000 // hz
            except (AttributeError, OSError, IndexError):
                return None

        def sample():
            k = 0
            while not done.is_set():
                now = time.time()
                samples.append((now, readstat(cg / 'cpu.stat')['usage_usec']))
                host.append((now, host_busy_usec(), client_usec()))
                if k % 4 == 0:
                    mi = meminfo()
                    st = sockstat()
                    ms = readstat(cg / 'memory.stat')
                    mem.append({'t': now, 'cg_current': int((cg / 'memory.current').read_text()),
                                'cg_anon': ms['anon'], 'cg_file': ms['file'], 'cg_kernel': ms.get('kernel', 0),
                                'cg_sock': ms['sock'], 'cg_slab': ms['slab'],
                                'MemAvailable': mi['MemAvailable'], 'Slab': mi['Slab'], 'SUnreclaim': mi['SUnreclaim'],
                                'tcp_inuse': st['TCP']['inuse'], 'tcp_mem_pages': st['TCP']['mem'],
                                'tcp_tw': st['TCP']['tw'], 'tcp_orphan': st['TCP']['orphan']})
                k += 1
                done.wait(.25)

        sampler = threading.Thread(target=sample)
        sampler.start()
        snaps = {}

        def snap():
            at = a.snap_at if a.snap_at is not None else a.ramp + a.duration / 2
            if done.wait(at):
                return
            snaps['hold'] = {'meminfo': meminfo(), 'slab': slabinfo(), 'sockstat': sockstat(),
                             'cg_stat': readstat(cg / 'memory.stat'), 'cg_current': int((cg / 'memory.current').read_text()),
                             'at_s': at}
        snapper = threading.Thread(target=snap)

        def stall():
            if done.wait(a.stall_at):
                return
            (cg / 'cgroup.freeze').write_text('1')
            time.sleep(a.stall_for)
            (cg / 'cgroup.freeze').write_text('0')
        staller = threading.Thread(target=stall) if a.stall_at is not None else None

        def profile():
            if done.wait(a.perf_at):
                return
            subprocess.run(['perf', 'record', '-q', '-C', '0', '-g', '-F', '499', '-o', str(out / 'perf.data'),
                            '--', 'sleep', str(a.perf_for)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for sort, name in (('comm,dso,sym', 'perf-sym.txt'), ('comm,dso', 'perf-dso.txt')):
                with (out / name).open('w') as f:
                    subprocess.run(['perf', 'report', '-i', str(out / 'perf.data'), '--no-children', '--sort', sort,
                                    '--stdio', '-g', 'none', '--percent-limit', '0.2'], stdout=f, stderr=subprocess.DEVNULL)
        profiler = threading.Thread(target=profile) if a.perf_at is not None else None
        sysc = {}

        def syscalls():
            if done.wait(a.syscalls_at):
                return
            pid = (cg / 'cgroup.procs').read_text().split()[0]
            r0 = len(samples)
            t0 = time.time()
            events = ['raw_syscalls:sys_enter', 'syscalls:sys_enter_io_uring_enter', 'syscalls:sys_enter_pwrite64',
                      'syscalls:sys_enter_pread64', 'syscalls:sys_enter_nanosleep', 'syscalls:sys_enter_clock_nanosleep',
                      'syscalls:sys_enter_epoll_wait', 'syscalls:sys_enter_futex', 'syscalls:sys_enter_fdatasync',
                      'syscalls:sys_enter_fsync', 'syscalls:sys_enter_fcntl', 'syscalls:sys_enter_newfstat', 'syscalls:sys_enter_lseek', 'cycles:u', 'cycles:k', 'instructions:u', 'instructions:k',
                      'branch-misses', 'cache-misses']
            res = subprocess.run(['perf', 'stat', '-x', ',', '-e', ','.join(events),
                                  '-p', pid, '--', 'sleep', '20'], capture_output=True, text=True)
            sysc['window'] = [t0, time.time()]
            sysc['perf'] = res.stderr.strip().splitlines()
        counter = threading.Thread(target=syscalls) if a.syscalls_at is not None else None
        hw = {}

        def hwcount():
            if done.wait(a.hw_at):
                return
            pid = (cg / 'cgroup.procs').read_text().split()[0]
            t0 = time.time()
            res = subprocess.run(['perf', 'stat', '-x', ',', '-e', 'cycles:u,cycles:k,instructions:u,instructions:k',
                                  '-p', pid, '--', 'sleep', str(a.hw_for)], capture_output=True, text=True)
            hw['window'] = [t0, time.time()]
            hw['perf'] = res.stderr.strip().splitlines()
        hwthread = threading.Thread(target=hwcount) if a.hw_at is not None else None
        try:
            client = r = subprocess.Popen(lg, stdout=(out / 'client.out').open('w'), stderr=(out / 'client.err').open('w'))
            snapper.start()
            if staller:
                staller.start()
            if profiler:
                profiler.start()
            if counter:
                counter.start()
            if hwthread:
                hwthread.start()
            r.wait()
        finally:
            done.set()
            sampler.join()
            snapper.join()
            if staller:
                staller.join()
            if profiler:
                profiler.join()
            if counter:
                counter.join()
            if hwthread:
                hwthread.join()
        (out / 'cpu-samples.json').write_text(json.dumps(samples))
        (out / 'host-samples.json').write_text(json.dumps(host))
        (out / 'mem-series.json').write_text(json.dumps(mem))
        result['exit_code'] = r.returncode
        result['cpu_stat'] = readstat(cg / 'cpu.stat')
        result['cpu_usec'] = result['cpu_stat']['usage_usec'] - u0
        d = json.loads((out / 'load.json').read_text())
        allreq = d['windows']['whole']['endpoints']['all']
        hold = d['windows']['hold']['endpoints']['all']

        def cpu_at(t):
            for (ta, ca), (tb, cb) in zip(samples, samples[1:]):
                if ta <= t <= tb:
                    return ca + (cb - ca) * (t - ta) / (tb - ta)
            raise RuntimeError('CPU samples do not cover the hold window')
        started = datetime.datetime.fromisoformat(d['started_at'].replace('Z', '+00:00')).timestamp()
        win = d['windows']['hold']
        result['hold_cpu_us_per_request'] = (cpu_at(started + win['end_s']) - cpu_at(started + win['start_s'])) / hold['count']
        result.update(requests=allreq['count'], errors=allreq['errors'],
                      cpu_us_per_request=result['cpu_usec'] / allreq['count'])
        result.update(load_details(d, samples, host, started))
        result['memory_peak_bytes'] = int((cg / 'memory.peak').read_text())
        result['memory_end'] = readstat(cg / 'memory.stat')
        if mem:
            result['min_MemAvailable_kB'] = min(m['MemAvailable'] for m in mem)
            result['max_tcp_mem_pages'] = max(m['tcp_mem_pages'] for m in mem)
            result['max_SUnreclaim_delta_kB'] = max(m['SUnreclaim'] for m in mem) - base['meminfo']['SUnreclaim']
        if 'hold' in snaps:
            h = snaps['hold']
            conns = h['sockstat']['TCP']['inuse'] - base['sockstat']['TCP']['inuse']
            result['hold_snapshot'] = {
                'at_s': h['at_s'], 'tcp_inuse_delta': conns,
                'cg_current_mb': round(h['cg_current'] / 2**20, 1),
                'cg_stat_mb': {k: round(h['cg_stat'][k] / 2**20, 1) for k in
                               ('anon', 'file', 'kernel', 'kernel_stack', 'pagetables', 'sock', 'slab',
                                'slab_reclaimable', 'slab_unreclaimable', 'percpu') if k in h['cg_stat']},
                'host_delta_kB': {k: h['meminfo'][k] - base['meminfo'][k] for k in
                                  ('MemAvailable', 'MemFree', 'Slab', 'SUnreclaim', 'SReclaimable', 'KernelStack',
                                   'PageTables', 'AnonPages', 'Cached')},
                'tcp_mem_pages': h['sockstat']['TCP']['mem'],
                'slab_delta': slab_delta(base['slab'], h['slab'], conns // 2 if conns else 0)}
        if sysc.get('perf'):
            counts = {}
            for line in sysc['perf']:
                f = line.split(',')
                if len(f) > 2 and f[0].strip().isdigit():
                    counts[f[2]] = int(f[0])
            t0, t1 = sysc['window']
            reqs = sum(t['reqs'] for t in d['timeline'] if t0 - started <= t['t'] <= t1 - started)
            result['syscalls'] = {'counts': counts, 'requests_in_window': reqs,
                                  'per_request': {k: v / reqs for k, v in counts.items()} if reqs else None}
        if hw.get('perf'):
            counts = {}
            for line in hw['perf']:
                f = line.split(',')
                if len(f) > 2 and f[0].strip().isdigit():
                    counts[f[2]] = int(f[0])
            t0, t1 = hw['window']
            reqs = sum(t['reqs'] for t in d['timeline'] if t0 - started <= t['t'] <= t1 - started)
            result['hw'] = {'counts': counts, 'requests_in_window': reqs,
                            'per_request': {k: v / reqs for k, v in counts.items()} if reqs else None}
        log = (out / 'server.log').read_text(errors='replace').splitlines()
        result['server_log_tail'] = log[-12:]
        (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
        brief = {k: result.get(k) for k in ('variant', 'tag', 'vus', 'cpu_quota', 'stall_at', 'exit_code', 'requests',
                                             'errors', 'hold_cpu_us_per_request', 'hold_p95_ms', 'hold_p99_ms',
                                             'whole_p95_ms', 'whole_p99_ms', 'hold_worst_second_p99_ms', 'max_conns',
                                             'min_hold_conns', 'memory_peak_bytes', 'min_MemAvailable_kB',
                                             'max_tcp_mem_pages', 'hold_server_cores', 'hold_other_host_cores')}
        brief['dir'] = str(out)
        print(json.dumps(brief), flush=True)
    finally:
        subprocess.run(['systemctl', 'stop', unit + '.scope'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            pass
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
