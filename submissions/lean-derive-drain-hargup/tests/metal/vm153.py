#!/usr/bin/env python3
"""vm153.py VARIANT [options] — one load run against a server inside a 1-vCPU / 2 GB KVM guest.

Why: the official box is a 1 vCPU / 2 GB DigitalOcean droplet driven by k6 over a real network.
The loopback harness (bench153.py) pins the server to one core of a 62 GB host, where receive
processing for loopback traffic runs on the sending (load generator) CPUs, kernel memory is
sized for 62 GB and idling costs nothing. Officially #264 used 25 µs/request and #153 31 µs
(leaderboard), a gap the loopback harness does not show. In the guest, the single vCPU also
does the virtio-net interrupt and softirq work, the guest kernel auto-sizes TCP memory for 2 GB,
and halting/waking and timers cost VM exits.

Setup (runs inside the l153-vmhost container, see runvm153.sh; build with vm_build.sh):
  qemu/KVM, -cpu host, 1 vCPU (thread pinned to host CPU 0; CPU 6, its SMT sibling, left idle),
  2048 MB, virtio-net on a tap device with vhost-net (qemu's other threads and vhost on CPUs 5,11),
  virtio-blk root disk opened with snapshot=on (every boot starts from the seed database), the
  host's Ubuntu 6.8 kernel. The guest's PID 1 is vm_init.sh. Load: the native generator
  (bench/load.js user model) on CPUs 1-4,7-10 from 40 source addresses on the tap's subnet.

CPU per request over the hold window, three ways:
  vcpu_us_per_request   — host view: the vCPU thread's run time (/proc/PID/task/TID/schedstat);
                          all guest work plus VM-exit handling. The capacity-relevant number.
  guest_*_us_per_request — guest view from /proc/stat: user, system, irq, softirq, busy.
  server_us_per_request — the server process's utime+stime inside the guest.
"""
import argparse, datetime, json, os, pathlib, re, resource, socket, subprocess, threading, time

ROOT = pathlib.Path(os.environ.get('STUDY_ROOT', '/root/twelve/lean-153-improve'))
VM = ROOT / 'vm'
CHALLENGE = pathlib.Path('/root/twelve/challenge')
GUEST, HOSTIP = '10.153.0.2', '10.153.0.1'


def schedstat_ns(pid, tid):
    return int(pathlib.Path(f'/proc/{pid}/task/{tid}/schedstat').read_text().split()[0])


def threads(pid):
    out = {}
    for t in pathlib.Path(f'/proc/{pid}/task').iterdir():
        try:
            out[int(t.name)] = (t / 'comm').read_text().strip()
        except OSError:
            pass
    return out


def host_busy_usec(hz):
    f = [int(x) for x in pathlib.Path('/proc/stat').read_text().split('\n', 1)[0].split()[1:]]
    return (sum(f[:8]) - f[3] - f[4]) * 1_000_000 // hz


def parse_stat_line(line):
    # "@S up | cpu  u n s i io irq sirq steal g gn | <proc stat after comm> | mem rss | TCP: inuse ..."
    parts = [p.strip() for p in line[3:].split('|')]
    if len(parts) < 5:
        return None
    up = float(parts[0])
    cpu = [int(x) for x in parts[1].split()[1:]]
    proc = parts[2].split()
    srv = (int(proc[11]) + int(proc[12])) if len(proc) > 12 else None
    mr = parts[3].split()
    tcp = parts[4].split()
    tcpd = {tcp[i]: int(tcp[i + 1]) for i in range(1, len(tcp) - 1, 2)} if tcp and tcp[0] == 'TCP:' else {}
    cg = {}
    if len(parts) > 5:
        c = parts[5].split()
        if len(c) >= 3:
            cg = {'current': int(c[0]), 'peak': int(c[1]), 'usage_usec': int(c[2])}
            cg.update({k: int(v) for k, v in (x.split('=') for x in c[3:])})
    return {'up': up, 'cpu': cpu, 'srv': srv, 'mem_avail_kB': int(mr[0]) if mr else None,
            'rss_kB': int(mr[1]) if len(mr) > 1 else None, 'tcp': tcpd, 'cg': cg}


def tapstat():
    d = pathlib.Path('/sys/class/net/tap0/statistics')
    return {k: int((d / k).read_text()) for k in ('rx_packets', 'tx_packets', 'rx_dropped', 'tx_dropped')}


NET_KEYS = {'TcpExt': ['ListenOverflows', 'ListenDrops', 'TCPBacklogDrop', 'PruneCalled', 'RcvPruned', 'OfoPruned',
                       'TCPMemoryPressures', 'TCPLostRetransmit', 'TCPTimeouts', 'TCPSynRetrans', 'TCPReqQFullDrop',
                       'TCPRcvQDrop', 'TCPZeroWindowDrop', 'TCPAbortOnMemory', 'TCPAbortOnTimeout', 'TCPRetransFail',
                       'TCPDelivered', 'DelayedACKs', 'TCPFastRetrans', 'TCPSlowStartRetrans', 'TCPSpuriousRTOs'],
            'Tcp': ['RetransSegs', 'InSegs', 'OutSegs', 'EstabResets', 'AttemptFails', 'InErrs', 'OutRsts']}


def interp(series, t):
    """series: [(time, value)], linear interpolation; clamped to the first/last sample (a guest
    whose server was killed, or that froze, stops reporting: the run is flagged, not lost)."""
    if not series:
        raise RuntimeError('no samples')
    if t <= series[0][0]:
        return series[0][1]
    if t >= series[-1][0]:
        return series[-1][1]
    for (ta, va), (tb, vb) in zip(series, series[1:]):
        if ta <= t <= tb:
            return va + (vb - va) * (t - ta) / (tb - ta) if tb > ta else va
    return series[-1][1]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('variant')
    p.add_argument('--vus', type=int, default=100000)
    p.add_argument('--ramp', type=int, default=60)
    p.add_argument('--duration', type=int, default=120)
    p.add_argument('--rampdown', type=int, default=30)
    p.add_argument('--mem', type=int, default=2048, help='guest memory, MB')
    p.add_argument('--kernel', default='6.8.0-149-generic')
    p.add_argument('--tag', default='')
    p.add_argument('--perf-at', type=int, help='guest profile from this many seconds after the load starts')
    p.add_argument('--perf-for', type=int, default=20)
    a = p.parse_args()
    hz = os.sysconf('SC_CLK_TCK')
    label = f'vm-{a.variant}{"-" + a.tag if a.tag else ""}-{time.time_ns()}'
    out = ROOT / 'results' / label
    out.mkdir(parents=True)
    result = {'variant': a.variant, 'tag': a.tag, 'directory': str(out), 'vus': a.vus, 'ramp': a.ramp,
              'duration': a.duration, 'rampdown': a.rampdown, 'guest_mem_mb': a.mem, 'guest_kernel': a.kernel,
              'host_kernel': os.uname().release, 'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()}
    subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=True)
    # guest uptime at load start is ~boot + 2 s; a 15 s margin keeps the profile inside the hold
    perf_args = ''
    if a.perf_at is not None:
        boot_margin = 15
        perf_args = (f' perf_at={a.perf_at + boot_margin} perf_for={a.perf_for}'
                     f' perf_report_at={a.ramp + a.duration + boot_margin + 5}')
    qemu = ['taskset', '-c', '5,11', 'qemu-system-x86_64', '-name', 'vm153,debug-threads=on',
            '-enable-kvm', '-cpu', 'host', '-smp', '1',
            '-m', str(a.mem), '-nodefaults', '-no-user-config', '-display', 'none', '-monitor', 'none',
            '-kernel', str(VM / f'vmlinuz-{a.kernel}'),
            '-append', f'console=ttyS0 root=/dev/vda rw init=/init.sh variant={a.variant} quiet' + perf_args,
            '-drive', f'file={VM / "rootfs.img"},if=virtio,format=raw,snapshot=on',
            '-netdev', 'tap,id=n0,ifname=tap0,script=no,downscript=no,vhost=on',
            '-device', 'virtio-net-pci,netdev=n0,mac=52:54:00:15:30:02',
            '-serial', f'file:{out}/console.log',
            '-device', 'virtio-serial-pci',
            '-chardev', f'file,id=c0,path={out}/stats.log', '-device', 'virtconsole,chardev=c0',
            '-chardev', f'file,id=c1,path={out}/server.log', '-device', 'virtconsole,chardev=c1',
            '-chardev', f'file,id=c2,path={out}/kmsg.log', '-device', 'virtconsole,chardev=c2',
            '-chardev', f'file,id=c3,path={out}/perf.txt', '-device', 'virtconsole,chardev=c3']
    vm = subprocess.Popen(qemu, stdout=(out / 'qemu.out').open('w'), stderr=subprocess.STDOUT)
    try:
        t = time.monotonic()
        while not pathlib.Path('/sys/class/net/tap0').exists():
            if vm.poll() is not None or time.monotonic() - t > 30:
                raise RuntimeError('qemu did not create tap0: ' + (out / 'qemu.out').read_text())
            time.sleep(.05)
        for cmd in (['ip', 'addr', 'add', HOSTIP + '/16', 'dev', 'tap0'], ['ip', 'link', 'set', 'tap0', 'up']):
            subprocess.run(cmd, check=True)
        for i in range(1, 41):
            subprocess.run(['ip', 'addr', 'add', f'10.153.1.{i}/16', 'dev', 'tap0'], check=True)
        for _ in range(100):      # thread names need -name ...,debug-threads=on
            ths = threads(vm.pid)
            vcpu = [tid for tid, c in ths.items() if c.startswith('CPU 0/KVM')]
            if vcpu:
                break
            time.sleep(.1)
        if len(vcpu) != 1:
            raise RuntimeError(f'vCPU thread not found: {ths}')
        vcpu = vcpu[0]
        os.sched_setaffinity(vcpu, {0})
        result['qemu_threads'] = sorted(set(ths.values()))
        ready = None
        while time.monotonic() - t < 120:
            txt = (out / 'stats.log').read_text(errors='replace') if (out / 'stats.log').exists() else ''
            if '@READY' in txt or '@FAIL' in txt:
                ready = '@READY' in txt
                break
            if vm.poll() is not None:
                break
            time.sleep(.1)
        if not ready:
            raise RuntimeError('guest server not ready: ' + ((out / 'stats.log').read_text(errors='replace')
                                                             if (out / 'stats.log').exists() else 'no stats'))
        result['boot_to_ready_s'] = time.monotonic() - t
        s = socket.create_connection((GUEST, 3000), timeout=5)
        s.sendall(b'GET /health HTTP/1.1\r\nHost: vm\r\n\r\n')
        if b' 200' not in s.recv(4096).split(b'\r\n', 1)[0]:
            raise RuntimeError('health over the tap failed')
        s.close()
        vhost = [tid for tid, c in threads(vm.pid).items() if c.startswith('vhost')]
        result['vhost_threads'] = len(vhost)
        time.sleep(2)
        resource.setrlimit(resource.RLIMIT_NOFILE, (1048576, 1048576))
        lg = ['/root/twelve/loadgen/loadgen', '--url', f'http://{GUEST}:3000',
              '--tokens', str(CHALLENGE / 'seed/tokens.json'), '--threads', '8', '--cpus', '1-4,7-10',
              '--out', str(out / 'load.json'), '--vus', str(a.vus), '--ramp', str(a.ramp),
              '--duration', str(a.duration), '--rampdown', str(a.rampdown), '--src', '10.153.1.1-10.153.1.40']
        samples, gl, nl, taps = [], [], [], []
        done = threading.Event()
        pos = [0]

        def sample():
            while not done.is_set():
                now = time.time()
                try:
                    v = schedstat_ns(vm.pid, vcpu)
                    vh = sum(schedstat_ns(vm.pid, x) for x in vhost)
                except OSError:
                    break
                samples.append((now, v, vh, host_busy_usec(hz)))
                taps.append((now, tapstat()))
                with (out / 'stats.log').open('rb') as f:
                    f.seek(pos[0])
                    chunk = f.read()
                cut = chunk.rfind(b'\n') + 1
                pos[0] += cut
                for line in chunk[:cut].decode(errors='replace').splitlines():
                    if line.startswith('@N '):
                        f = [x.strip() for x in line[3:].split('|')]
                        if len(f) >= 4:
                            nl.append({'up': float(f[0]), 'nx': f[1].split(), 'nt': f[2].split(),
                                       'sn': [int(x, 16) for x in f[3].split()],
                                       'ks': f[4].split() if len(f) > 4 else []})
                    if line.startswith('@S '):
                        r = parse_stat_line(line)
                        if r:
                            r['arrived'] = now
                            gl.append(r)
                done.wait(.25)

        sampler = threading.Thread(target=sample)
        sampler.start()
        try:
            client = subprocess.Popen(lg, stdout=(out / 'client.out').open('w'), stderr=(out / 'client.err').open('w'))
            client.wait()
        finally:
            done.set()
            sampler.join()
        (out / 'vcpu-samples.json').write_text(json.dumps(samples))
        (out / 'guest-samples.json').write_text(json.dumps(gl))
        (out / 'net-samples.json').write_text(json.dumps(nl))
        (out / 'tap-samples.json').write_text(json.dumps(taps))
        result['exit_code'] = client.returncode
        d = json.loads((out / 'load.json').read_text())
        cfg = d['config']
        whole = d['windows']['whole']['endpoints']['all']
        hold = d['windows']['hold']['endpoints']['all']
        tl = d['timeline']
        hold_tl = [x for x in tl if cfg['ramp_s'] < x['t'] <= cfg['ramp_s'] + cfg['duration_s']]
        started = datetime.datetime.fromisoformat(d['started_at'].replace('Z', '+00:00')).timestamp()
        t0, t1 = started + d['windows']['hold']['start_s'], started + d['windows']['hold']['end_s']
        n = hold['count']
        result.update(verdict=d['verdict'], requests=whole['count'], errors=whole['errors'],
                      whole_error_rate=whole['error_rate'], whole_errors_by_kind=whole['errors_by_kind'],
                      hold_requests=n, hold_errors_by_kind=hold['errors_by_kind'],
                      hold_p95_ms=hold['total_ms']['p95'], hold_p99_ms=hold['total_ms']['p99'],
                      hold_p999_ms=hold['total_ms']['p999'], hold_max_ms=hold['total_ms']['max'],
                      whole_p95_ms=whole['total_ms']['p95'], whole_p99_ms=whole['total_ms']['p99'],
                      hold_worst_second_p99_ms=max((x['p99_ms'] for x in hold_tl), default=None),
                      hold_mean_rps=sum(x['reqs'] for x in hold_tl) / max(1, len(hold_tl)),
                      max_conns=max((x['conns'] for x in tl), default=None),
                      min_hold_conns=min((x['conns'] for x in hold_tl if x['t'] > cfg['ramp_s'] + 6), default=None),
                      counters=d['counters'])
        span = t1 - t0
        vser = [(x[0], x[1]) for x in samples]
        hser = [(x[0], x[2]) for x in samples]
        bser = [(x[0], x[3]) for x in samples]
        result['vcpu_us_per_request'] = (interp(vser, t1) - interp(vser, t0)) / 1000 / n
        result['vcpu_busy_fraction'] = (interp(vser, t1) - interp(vser, t0)) / 1e9 / span
        result['vhost_us_per_request'] = (interp(hser, t1) - interp(hser, t0)) / 1000 / n
        result['host_busy_cores'] = (interp(bser, t1) - interp(bser, t0)) / 1e6 / span
        kmsg_all = (out / 'kmsg.log').read_text(errors='replace') if (out / 'kmsg.log').exists() else ''
        result['oom_killed'] = 'invoked oom-killer' in kmsg_all or 'Out of memory: Killed' in kmsg_all
        if gl:
            off = min(g['arrived'] - g['up'] for g in gl)          # guest uptime -> host time
            last_srv = max((g['up'] + off for g in gl if g['srv'] is not None), default=None)
            result['server_alive_through_hold'] = last_srv is not None and last_srv >= t1 - 2
            result['server_last_seen_s_after_start'] = (last_srv - started) if last_srv else None
            # the app cgroup's memory just before the server stopped reporting (or at the end)
            live = [g for g in gl if g['srv'] is not None and g.get('cg')]
            if live:
                lg_ = live[-1]
                result['app_cg_last_mb'] = {k: round(v / 2**20, 1) for k, v in lg_['cg'].items() if k != 'usage_usec'}
                result['guest_last_tcp'] = lg_['tcp']
                result['app_cg_max_sock_mb'] = max(g['cg'].get('sock', 0) for g in live) / 2**20
                result['guest_max_tcp_mem_pages_live'] = max(g['tcp'].get('mem', 0) for g in live)
            gt = [(g['up'] + off, g) for g in gl]
            names = ['user', 'nice', 'system', 'idle', 'iowait', 'irq', 'softirq', 'steal']

            def gcpu(i):
                return [(tt, g['cpu'][i] * 1e6 / hz) for tt, g in gt]
            for i, nm in enumerate(names):
                result[f'guest_{nm}_us_per_request'] = (interp(gcpu(i), t1) - interp(gcpu(i), t0)) / n
            result['guest_busy_us_per_request'] = sum(result[f'guest_{k}_us_per_request'] for k in
                                                      ('user', 'nice', 'system', 'irq', 'softirq', 'steal'))
            sv = [(tt, g['srv'] * 1e6 / hz) for tt, g in gt if g['srv'] is not None]
            if len(sv) > 1:
                result['server_us_per_request'] = (interp(sv, t1) - interp(sv, t0)) / n
            hold_g = [g for tt, g in gt if t0 <= tt <= t1]
            cu = [(tt, g['cg']['usage_usec']) for tt, g in gt if g.get('cg')]
            if len(cu) > 1:
                # the leaderboard's columns: app cgroup CPU over the hold, and the cgroup's memory peak
                result['app_cg_us_per_request'] = (interp(cu, t1) - interp(cu, t0)) / n
                result['app_cg_cpu_fraction'] = (interp(cu, t1) - interp(cu, t0)) / 1e6 / span
                result['app_cg_peak_mb'] = max(g['cg']['peak'] for tt, g in gt if g.get('cg')) / 2**20
                mid = hold_g[len(hold_g) // 2] if hold_g else None
                if mid and mid.get('cg'):
                    result['app_cg_hold_mid_mb'] = {k: round(v / 2**20, 1) for k, v in mid['cg'].items()
                                                    if k not in ('usage_usec',)}
            result['guest_min_mem_avail_mb'] = min((g['mem_avail_kB'] for g in hold_g if g['mem_avail_kB']), default=0) / 1024
            result['guest_max_rss_mb'] = max((g['rss_kB'] or 0 for g in gl), default=0) / 1024
            result['guest_max_tcp_mem_pages'] = max((g['tcp'].get('mem', 0) for g in gl), default=0)
            result['guest_max_tcp_inuse'] = max((g['tcp'].get('inuse', 0) for g in gl), default=0)
        if gl and nl:
            txt = (out / 'stats.log').read_text(errors='replace')
            hx = next((l.split()[2:] for l in txt.splitlines() if l.startswith('@NXH ')), [])
            ht = next((l.split()[2:] for l in txt.splitlines() if l.startswith('@NTH ')), [])
            nh = [(x['up'] + off, x) for x in nl]
            a = min(nh, key=lambda z: abs(z[0] - t0))[1]
            b = min(nh, key=lambda z: abs(z[0] - t1))[1]
            span_n = (b['up'] - a['up']) or 1
            net = {}
            for hdr, key, keys in ((hx, 'nx', NET_KEYS['TcpExt']), (ht, 'nt', NET_KEYS['Tcp'])):
                for k in keys:
                    if k in hdr and len(a[key]) == len(hdr) == len(b[key]):
                        i = hdr.index(k)
                        net[k] = int(b[key][i]) - int(a[key][i])
            net['softnet_processed'] = b['sn'][0] - a['sn'][0]
            net['softnet_dropped'] = b['sn'][1] - a['sn'][1]
            net['softnet_time_squeeze'] = b['sn'][2] - a['sn'][2]
            if len(a['ks']) > 12 and len(b['ks']) > 12:
                net['ksoftirqd_cpu_fraction'] = ((int(b['ks'][11]) + int(b['ks'][12])) - (int(a['ks'][11]) + int(a['ks'][12]))) / hz / span_n
            net['window_s'] = span_n
            result['guest_net_hold'] = net
            result['net_retrans_per_1k_requests'] = net.get('RetransSegs', 0) * 1000 / n
        if len(taps) > 1:
            ta = min(taps, key=lambda z: abs(z[0] - t0))[1]
            tb = min(taps, key=lambda z: abs(z[0] - t1))[1]
            result['tap_hold'] = {k: tb[k] - ta[k] for k in ta}
        kmsg = (out / 'kmsg.log').read_text(errors='replace').splitlines() if (out / 'kmsg.log').exists() else []
        result['kmsg_notable'] = [k for k in kmsg if re.search(r'oom|Out of|out of memory|SYN flood|dropping|killed|BUG|WARN', k, re.I)][:20]
        result['server_log_tail'] = (out / 'server.log').read_text(errors='replace').splitlines()[-12:]
        (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
        brief = {k: result.get(k) for k in ('variant', 'tag', 'vus', 'exit_code', 'verdict', 'requests', 'errors',
                                             'hold_p95_ms', 'hold_p99_ms', 'hold_worst_second_p99_ms', 'hold_mean_rps',
                                             'vcpu_us_per_request', 'vcpu_busy_fraction', 'guest_busy_us_per_request',
                                             'guest_user_us_per_request', 'guest_system_us_per_request',
                                             'guest_softirq_us_per_request', 'guest_irq_us_per_request',
                                             'server_us_per_request', 'vhost_us_per_request', 'guest_min_mem_avail_mb',
                                             'guest_max_rss_mb', 'guest_max_tcp_mem_pages', 'min_hold_conns',
                                             'app_cg_us_per_request', 'app_cg_cpu_fraction', 'app_cg_peak_mb',
                                             'app_cg_hold_mid_mb')}
        brief['kmsg_notable'] = len(result['kmsg_notable'])
        for k in ('oom_killed', 'server_alive_through_hold', 'server_last_seen_s_after_start', 'app_cg_max_sock_mb',
                  'guest_max_tcp_mem_pages_live', 'app_cg_last_mb'):
            brief[k] = result.get(k)
        g = result.get('guest_net_hold', {})
        brief['net'] = {k: g.get(k) for k in ('RetransSegs', 'TCPTimeouts', 'TCPLostRetransmit', 'ListenOverflows', 'TCPBacklogDrop',
                                              'PruneCalled', 'TCPMemoryPressures', 'softnet_dropped', 'softnet_time_squeeze',
                                              'ksoftirqd_cpu_fraction') if k in g}
        brief['tap_tx_dropped'] = result.get('tap_hold', {}).get('tx_dropped')
        brief['dir'] = str(out)
        print(json.dumps(brief), flush=True)
    finally:
        vm.terminate()
        try:
            vm.wait(timeout=20)
        except subprocess.TimeoutExpired:
            vm.kill()


if __name__ == '__main__':
    main()
