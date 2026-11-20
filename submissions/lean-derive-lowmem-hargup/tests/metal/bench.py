#!/usr/bin/env python3
"""Isolated benchmark harness; CPU/memory restrictions apply only to test processes."""
import argparse, datetime, json, os, pathlib, resource, re, shutil, socket, sqlite3, subprocess, sys, time, threading, tempfile
ROOT = pathlib.Path('/root/twelve/opt-audit-1024/metal')
CHALLENGE = pathlib.Path('/root/twelve/challenge')

def readstat(path):
    return {k: int(v) for k, v in (line.split() for line in path.read_text().splitlines())}

def main():
    p = argparse.ArgumentParser()
    p.add_argument('variant')
    p.add_argument('mode', choices=['check', 'overflow', 'load', 'fragment', 'blast'])
    p.add_argument('--vus', type=int, default=50000)
    p.add_argument('--ramp', type=int, default=15)
    p.add_argument('--duration', type=int, default=30)
    p.add_argument('--mix', default='mixed')
    p.add_argument('--conns', type=int, default=32)
    p.add_argument('--unprivileged', action='store_true')
    a = p.parse_args()
    label = f'{a.variant}-{a.mode}-{time.time_ns()}'
    out = ROOT/'results'/label
    out.mkdir(parents=True)
    shutil.copyfile(CHALLENGE/'seed/feed.db', out/'feed.db')
    subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=True)
    unit = 'opt-'+label
    cg = pathlib.Path('/sys/fs/cgroup/system.slice')/(unit+'.scope')
    nonroot_dir = None
    binary = ROOT/'artifacts'/a.variant/'server'
    db_path = out/'feed.db'
    identity = []
    if a.mode == 'overflow':
        with sqlite3.connect(db_path) as fixture:
            fixture.execute('PRAGMA journal_mode=DELETE')
            fixture.execute('DELETE FROM likes WHERE post_id > 30')
            fixture.execute('DELETE FROM posts WHERE id > 30')
            fixture.commit()
            fixture.execute('PRAGMA page_size=512')
            fixture.execute('VACUUM')
            uid = fixture.execute('SELECT user_id FROM posts ORDER BY id DESC LIMIT 1').fetchone()[0]
            fixture.execute('UPDATE users SET username=? WHERE id=?', ('author' * 4000, uid))
            fixture.execute('UPDATE posts SET body=? WHERE id=(SELECT max(id) FROM posts)', ('😀' * 500,))
    if a.unprivileged:
        nonroot_dir = pathlib.Path(tempfile.mkdtemp(prefix='twelve-opt-nonroot-'))
        os.chown(nonroot_dir, 65534, 65534)
        shutil.move(db_path, nonroot_dir/'feed.db')
        db_path = nonroot_dir/'feed.db'
        os.chown(db_path, 65534, 65534)
        shutil.copyfile(binary, nonroot_dir/'server')
        binary = nonroot_dir/'server'
        binary.chmod(0o755)
        identity = ['setpriv','--reuid=65534','--regid=65534','--clear-groups']
    cmd = ['systemd-run', '--quiet', '--scope', '--unit='+unit,
           '-p', 'AllowedCPUs=0', '-p', 'MemoryMax=2G', '-p', 'MemorySwapMax=0',
           'prlimit', '--nofile=65535:65535'] + identity + ['env', 'SQLITE_PATH='+str(db_path),
           'JWT_SECRET=twelve-dollar-challenge', 'HOST=127.0.0.1', 'PORT=3000',
           str(binary)]
    proc = subprocess.Popen(cmd, stdout=(out/'server.log').open('w'), stderr=subprocess.STDOUT)
    result = {'variant': a.variant, 'mode': a.mode, 'directory': str(out), 'unprivileged': a.unprivileged}
    try:
        t = time.monotonic()
        while time.monotonic()-t < 60:
            if subprocess.run(['curl', '-sf', '--max-time', '1', 'http://127.0.0.1:3000/health'],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
                break
            if proc.poll() is not None:
                raise RuntimeError((out/'server.log').read_text())
            time.sleep(.05)
        else:
            raise RuntimeError('startup timeout')
        result['startup_s'] = time.monotonic()-t
        if a.mode == 'overflow':
            assert 'Feed.Db.Inv: holds' in (out/'server.log').read_text(), 'fixture must use native feed'
            subprocess.run(['python3', str(ROOT/'source/tests/metal/oracle.py'), str(db_path), '1000'], check=True)
            with sqlite3.connect(db_path) as foreign:
                foreign.execute("UPDATE posts SET created_at='outside-format' WHERE id=(SELECT max(id) FROM posts)")
            subprocess.run(['python3', str(ROOT/'source/tests/metal/oracle.py'), str(db_path), '1000'], check=True)
        elif a.mode == 'check':
            subprocess.run(['python3', str(ROOT/'source/tests/metal/oracle.py'), str(CHALLENGE/'seed/feed.db')], check=True)
            r = subprocess.run(['bash', str(CHALLENGE/'test/test.sh'), 'http://127.0.0.1:3000'],
                               stdout=(out/'correctness.log').open('w'), stderr=subprocess.STDOUT)
            print((out/'correctness.log').read_text().splitlines()[-1], flush=True)
            if r.returncode:
                raise RuntimeError((out/'correctness.log').read_text())
            r = subprocess.run(['python3', str(ROOT/'source/tests/edge_cases.py')],
                               stdout=(out/'edge-cases.log').open('w'), stderr=subprocess.STDOUT)
            if r.returncode:
                raise RuntimeError((out/'edge-cases.log').read_text())
            print((out/'edge-cases.log').read_text().strip(), flush=True)
            subprocess.run(['python3', str(ROOT/'source/tests/metal/claims_cases.py')], check=True)
        elif a.mode == 'fragment':
            subprocess.run(['python3', '/root/twelve/opt-audit-1024/fragment.py', str(cg), str(out)], check=True)
            result.update(json.loads((out/'fragment.json').read_text()))
        else:
            u0 = readstat(cg/'cpu.stat')['usage_usec']
            if a.mode == 'load':
                resource.setrlimit(resource.RLIMIT_NOFILE, (1048576,1048576))
                cmd = ['/root/twelve/loadgen/loadgen', '--url', 'http://127.0.0.1:3000',
                       '--tokens', str(CHALLENGE/'seed/tokens.json'), '--threads', '8',
                       '--cpus', '1-5,7-11', '--out', str(out/'load.json'),
                       '--vus', str(a.vus), '--ramp', str(a.ramp), '--duration', str(a.duration),
                       '--rampdown', '5', '--src', '127.0.1.1-127.0.1.40']
            else:
                token = json.loads((CHALLENGE/'seed/tokens.json').read_text())[0]['token']
                cmd = ['taskset', '-c', '1-5,7-11', '/root/twelve/opt-audit-1024/blast', '127.0.0.1', '3000',
                       '4', str(a.conns), str(a.duration), a.mix, token]
            samples = []
            interference = set()
            client = None
            def check_interference():
                for procpath in pathlib.Path('/proc').glob('[0-9]*/cmdline'):
                    try:
                        argv = procpath.read_bytes().split(b'\0')
                    except (OSError, ProcessLookupError):
                        continue
                    exe = argv[0].rsplit(b'/',1)[-1]
                    own = (client is not None and procpath.parent.name == str(client.pid)) or str(out/'load.json').encode() in argv
                    if exe in (b'loadgen',b'blast',b'cc1') and not own:
                        interference.add(str(procpath.parent.name))
            check_interference()
            if interference:
                raise RuntimeError('Another load generator is active: '+str(interference))
            done = threading.Event()
            def sample():
                while not done.is_set():
                    samples.append((time.time(), readstat(cg/'cpu.stat')['usage_usec']))
                    if len(samples) % 4 == 0: check_interference()
                    done.wait(.25)
            sampler = threading.Thread(target=sample)
            sampler.start()
            try:
                client = r = subprocess.Popen(cmd, stdout=(out/'client.out').open('w'), stderr=(out/'client.err').open('w'))
                r.wait()
            finally:
                done.set()
                sampler.join()
            (out/'cpu-samples.json').write_text(json.dumps(samples))
            result['external_loadgen_pids'] = sorted(interference)
            result['performance_valid'] = not interference
            result['exit_code'] = r.returncode
            result['cpu_usec'] = readstat(cg/'cpu.stat')['usage_usec']-u0
            if a.mode == 'load':
                d = json.loads((out/'load.json').read_text())
                allreq = d['windows']['whole']['endpoints']['all']
                hold = d['windows']['hold']['endpoints']['all']
                def cpu_at(t):
                    for (ta, ca), (tb, cb) in zip(samples, samples[1:]):
                        if ta <= t <= tb:
                            return ca + (cb-ca)*(t-ta)/(tb-ta)
                    raise RuntimeError('CPU samples do not cover the hold window')
                started = datetime.datetime.fromisoformat(d['started_at'].replace('Z','+00:00')).timestamp()
                win = d['windows']['hold']
                result['hold_cpu_us_per_request'] = (cpu_at(started+win['end_s'])-cpu_at(started+win['start_s']))/hold['count']
                result.update(requests=allreq['count'], errors=allreq['errors'],
                              cpu_us_per_request=result['cpu_usec']/allreq['count'],
                              hold_p95_ms=hold['total_ms']['p95'], hold_p99_ms=hold['total_ms']['p99'])
            else:
                result['output'] = (out/'client.out').read_text().strip()
                counts = [int(x) for x in re.findall(r'(?:2xx|4xx|5xx|other) (\d+)', result['output'])]
                result['requests'] = sum(counts)
                result['errors'] = sum(counts[1:])
                result['cpu_us_per_request'] = result['cpu_usec']/result['requests']
        result['memory_peak_bytes'] = int((cg/'memory.peak').read_text())
        result['memory'] = readstat(cg/'memory.stat')
        (out/'summary.json').write_text(json.dumps(result, indent=2)+'\n')
        print(json.dumps({k:v for k,v in result.items() if k != 'memory'}), flush=True)
    finally:
        subprocess.run(['systemctl', 'stop', unit+'.scope'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        proc.wait(timeout=10)
        if nonroot_dir is not None:
            shutil.rmtree(nonroot_dir)
        for f in out.glob('feed.db*'):
            f.unlink()

if __name__ == '__main__':
    main()
