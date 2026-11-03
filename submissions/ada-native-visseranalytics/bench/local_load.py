#!/usr/bin/env python3
"""Synchronized k6 execution segments; workload and server limits stay unchanged."""
import concurrent.futures
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import threading
import time

ADA = Path(__file__).resolve().parents[1]
ROOT = ADA.parents[1]


def run(*args, check=True):
    result = subprocess.run(args, text=True, capture_output=True)
    if check and result.returncode:
        raise RuntimeError(f"{args[:3]} exited {result.returncode}: {result.stderr}")
    return result


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def inspect(name):
    return json.loads(run("docker", "inspect", name).stdout)[0]


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project")
    parser.add_argument("--vus", type=int, default=int(os.environ.get("VUS", "1000")))
    parser.add_argument("--duration", default=os.environ.get("DURATION", "30s"))
    parser.add_argument("--ramp-up", default=os.environ.get("RAMP_UP", "60s"))
    parser.add_argument("--shards", type=int, default=int(os.environ.get("K6_SHARDS", "2")))
    parser.add_argument("--memory-gb", type=int, default=int(os.environ.get("K6_MEMORY_GB", "4")))
    parser.add_argument("--memory-mib", type=int, help="exact per-shard memory limit with --isolated")
    parser.add_argument("--isolated", action="store_true", help="share a network_mode:none server namespace")
    parser.add_argument("--cpus", type=float, default=float(os.environ.get("K6_CPUS", "3")))
    parser.add_argument("--gomemlimit", default=os.environ.get("K6_GOMEMLIMIT"))
    parser.add_argument("--gogc", default=os.environ.get("K6_GOGC", "100"))
    parser.add_argument("--gc-trace", action="store_true", help="retain Go GC diagnostics in generator logs")
    parser.add_argument("--summary-only", action="store_true", help="omit raw sample export; retain per-shard k6 thresholds and summaries")
    args = parser.parse_args()
    project, shards, vus, memory, cpus = args.project, args.shards, args.vus, args.memory_gb, args.cpus
    memory_bytes = (args.memory_mib * 1024**2 if args.memory_mib else memory * 1024**3)
    if args.memory_mib and (not args.isolated or args.memory_mib < 2048):
        raise ValueError("--memory-mib requires --isolated and at least 2048 MiB")
    if not (1 <= shards <= 8 and vus >= shards and vus % shards == 0 and memory >= 2 and cpus > 0):
        raise ValueError("Use 1–8 shards, divisible positive VUS, >=2 GiB and positive CPUs PER SHARD")
    vm = json.loads(run("docker", "info", "--format", "{{json .}}").stdout)
    if vm["MemTotal"] < shards * memory_bytes + 3 * 1024**3:
        raise ValueError("VM needs sum of generator limits + 2 GiB server + 1 GiB headroom")
    os.environ.update(K6_MEMORY_GB=str(memory), K6_CPUS=str(cpus),
                      K6_GOMEMLIMIT=args.gomemlimit or f"{memory_bytes // 1024**2 - 1024}MiB",
                      ADA_LOAD_PLATFORM="linux/arm64" if vm["Architecture"] in ("aarch64", "arm64") else "linux/amd64")
    compose_file = ADA / ("compose.isolated.yaml" if args.isolated else "compose.local.yaml")
    compose = ["docker", "compose", "--project-name", project, "--file", str(compose_file)]
    server = run(*compose, "ps", "--quiet", "server").stdout.strip()
    if not server:
        raise RuntimeError("Start the server first")
    run("docker", "exec", server, "curl", "-fsS", "http://127.0.0.1/health")
    # Tie the measured binary to the worktree before recording source hashes.
    manifest_result = run("docker", "exec", server, "cat", "/app/build-sha256.txt", check=False)
    if manifest_result.returncode:
        raise RuntimeError("Server has no build manifest; rebuild it with local.sh up")
    manifest = manifest_result.stdout
    built = {line.split(None, 1)[1]: line.split(None, 1)[0] for line in manifest.splitlines()}
    expected = sorted((ADA / "src").glob("*")) + [ADA / "build.sh", ADA / "versions.env"]
    if built != {str(p.relative_to(ADA)): sha256(p) for p in expected}:
        raise RuntimeError("Server binary does not match current sources; rebuild with local.sh up")
    ident = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + f"-vu{vus}-shards{shards}-{os.getpid()}"
    result = ADA / "benchmark-results/local" / ident
    result.mkdir(parents=True)
    os.environ["ADA_LOAD_RESULTS"] = str(result)
    server_before = inspect(server)
    server_env = dict(item.split("=", 1) for item in server_before["Config"]["Env"])
    os.environ["ADA_SERVE_MODE"] = server_env.get("ADA_SERVE_MODE", "nginx")
    (result / "compose.yaml").write_text(run(*compose, "--profile", "load", "config").stdout)
    write_json(result / "server.json", [server_before])
    write_json(result / "docker-vm.json", {key: vm.get(key) for key in
               ("NCPU", "MemTotal", "Architecture", "ServerVersion", "OperatingSystem", "KernelVersion")})
    runtime_probe = r'''
import json, os
from pathlib import Path
pid = int(Path('/run/ada/app.pid').read_text())
proc = Path('/proc') / str(pid)
print(json.dumps(dict(pid=pid, executable=os.readlink(proc / 'exe'),
    emulator_mappings=[s for s in (proc / 'maps').read_text().splitlines()
                       if 'rosetta' in s.lower() or 'qemu' in s.lower()],
    fd_limit=[s for s in (proc / 'limits').read_text().splitlines() if 'Max open files' in s],
    cgroup={name: (Path('/sys/fs/cgroup') / name).read_text().strip()
            for name in ('cpu.max', 'memory.max', 'memory.swap.max')})))
'''
    write_json(result / "server-runtime.json", json.loads(run("docker", "exec", "--user", "10001:10001",
               server, "python3", "-c", runtime_probe).stdout))
    (result / "build-sha256.txt").write_text(manifest)
    paths = sorted((ADA / "src").glob("*")) + [ADA / "build.sh", ADA / "bench/entrypoint.sh",
            ADA / "bench/local_load.py", ADA / "bench/analyze_shards.py", ADA / "bench/monitor.py", ADA / "compose.local.yaml",
            ADA / "Dockerfile", ADA / "Dockerfile.cached", ADA / "versions.env", ADA / "install.sh", ADA / "start.sh", ADA / "local.sh",
            ROOT / "seed/feed.db", ROOT / "seed/tokens.json", ROOT / "bench/load.js", ROOT / "bench/nginx.conf",
            ADA / "compose.isolated.yaml"]
    write_json(result / "sha256.json", {str(p.relative_to(ROOT)): sha256(p) for p in paths})
    for path in paths:
        if path.parent == ROOT / "seed":
            continue  # Large shared inputs remain identified by their hashes.
        saved = result / "source" / path.relative_to(ROOT)
        saved.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, saved)
    sequence = ",".join(["0"] + [f"{i}/{shards}" for i in range(1, shards)] + ["1"])
    jobs = [f"{project}-load-{ident}-{i}" for i in range(shards)]
    plan = dict(target_vus=vus, shards=shards, vus_per_shard=vus // shards,
                ramp_up=args.ramp_up, duration=args.duration,
                cpus_per_shard=cpus, memory_bytes_per_shard=memory_bytes, isolated=args.isolated,
                gomemlimit=os.environ["K6_GOMEMLIMIT"], gogc=args.gogc,
                execution_segment_sequence=sequence, jobs=jobs, gc_trace=args.gc_trace,
                summary_only=args.summary_only)
    write_json(result / "plan.json", plan)
    stop = threading.Event()
    monitor = None
    stats_thread = None
    pid_file = f"/run/ada/monitor-{ident}.pid"

    def interrupt(signum, _frame):
        raise InterruptedError(f"signal {signum}")

    signal.signal(signal.SIGINT, interrupt)
    signal.signal(signal.SIGTERM, interrupt)

    def launch(i):
        directory = result / f"shard-{i}"
        directory.mkdir()
        write_json(directory / "config.json", {})
        low = "0" if i == 0 else f"{i}/{shards}"
        high = "1" if i + 1 == shards else f"{i + 1}/{shards}"
        if args.isolated:
            launch_command = ["docker", "run", "--detach", "--name", jobs[i],
                "--platform", os.environ["ADA_LOAD_PLATFORM"], "--user", "0:0",
                "--network", "container:" + server, "--cpus", str(cpus),
                "--memory", str(memory_bytes), "--memory-swap", str(memory_bytes),
                "--ulimit", "nofile=65535:65535", "-e", "GOMEMLIMIT=" + plan["gomemlimit"],
                "--mount", f"type=bind,source={ROOT / 'bench/load.js'},target=/scripts/load.js,readonly",
                "--mount", f"type=bind,source={ROOT / 'seed/tokens.json'},target=/seed/tokens.json,readonly",
                "--mount", f"type=bind,source={result},target=/results"]
        else:
            launch_command = compose + ["run", "--detach", "--no-deps", "--name", jobs[i]]
        run(*launch_command,
            "-e", f"GOGC={plan['gogc']}", "-e", "GODEBUG=" + ("gctrace=1" if args.gc_trace else ""),
            "grafana/k6:1.8.2" if args.isolated else "load", "run", "--quiet", "--paused",
            "--address", f"127.20.{i}.1:6565" if args.isolated else "0.0.0.0:6565",
            *(["--local-ips", f"127.10.{i}.2,127.10.{i}.3"] if args.isolated else []),
            "--execution-segment", f"{low}:{high}", "--execution-segment-sequence", sequence,
            "--config", f"/results/shard-{i}/config.json", "--summary-export", f"/results/shard-{i}/summary.json",
            *([] if args.summary_only else ["--out", f"json=/results/shard-{i}/samples.json.gz"]),
            "-e", "BASE_URL=http://127.0.0.1" if args.isolated else "BASE_URL=http://server",
            "-e", "TOKENS=/seed/tokens.json",
            "-e", f"VUS={vus}", "-e", f"RAMP_UP={plan['ramp_up']}", "-e", f"DURATION={plan['duration']}", "/scripts/load.js")

    def sample():
        with (result / "stats.jsonl").open("w") as out, (result / "stats.err").open("w") as err, \
             (result / "host-memory.jsonl").open("w") as host:
            while not stop.is_set():
                sampled_at = dt.datetime.now(dt.timezone.utc).isoformat()
                proc = run("docker", "stats", "--no-stream", "--format", "{{json .}}", server, *jobs, check=False)
                err.write(proc.stderr)
                for line in proc.stdout.splitlines():
                    try:
                        out.write(json.dumps(dict(sampled_at=sampled_at, stats=json.loads(line))) + "\n")
                    except ValueError:
                        err.write(line + "\n")
                out.flush()
                if sys.platform == "darwin":
                    host.write(json.dumps(dict(sampled_at=sampled_at,
                        vm_stat=run("vm_stat", check=False).stdout,
                        swap=run("sysctl", "vm.swapusage", check=False).stdout)) + "\n")
                    host.flush()
                stop.wait(3)

    code = 1
    try:
        print(f"Results: {result}\nPreparing {shards} generators, {vus // shards} VUs each", flush=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=shards) as pool:
            list(pool.map(launch, range(shards)))
        for i, job in enumerate(jobs):
            (result / f"shard-{i}/ephemeral-ports.txt").write_text(
                run("docker", "exec", job, "cat", "/proc/sys/net/ipv4/ip_local_port_range").stdout)
        stats_thread = threading.Thread(target=sample)
        stats_thread.start()
        monitor = subprocess.Popen([
            "docker", "exec", server, "sh", "-c",
            'set -- --app-pid "$(cat /run/ada/app.pid)"; '
            'if [ -f /run/ada/nginx.pid ]; then set -- "$@" --nginx-pid "$(cat /run/ada/nginx.pid)"; fi; '
            'exec python3 /app/bench/monitor.py "$@" --db /data/feed.db --interval 5 --samples 240 --pid-file "$1_PID"'
            .replace('"$1_PID"', '"' + pid_file + '"')],
            stdout=(result / "server-monitor.jsonl").open("w"), stderr=(result / "server-monitor.err").open("w"))
        # REST is accessible only inside this checkout's Docker network.
        # Wait for initialized, paused engines, then resume them concurrently.
        sync = r'''
import concurrent.futures, datetime, json, sys, time, urllib.request
jobs = json.loads(sys.argv[1])
target = int(sys.argv[2])
def status(job):
    with urllib.request.urlopen('http://' + job + ':6565/v1/status', timeout=3) as r:
        return json.load(r)['data']['attributes']
deadline = time.monotonic() + 180
while True:
    try:
        states = [status(job) for job in jobs]
        if all(str(s.get('paused')).lower() == 'true' and int(s.get('vus-max', 0)) >= target for s in states): break
    except OSError: pass
    if time.monotonic() >= deadline: raise TimeoutError('Generators did not initialize paused')
    time.sleep(.25)
def resume(job):
    started = datetime.datetime.now(datetime.timezone.utc).isoformat()
    req = urllib.request.Request('http://' + job + ':6565/v1/status', method='PATCH',
        data=json.dumps({'data': {'type': 'status', 'id': 'default', 'attributes': {'paused': False}}}).encode(),
        headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=5) as r: response = json.load(r)
    return dict(job=job, requested_at=started, completed_at=datetime.datetime.now(datetime.timezone.utc).isoformat(), response=response)
with concurrent.futures.ThreadPoolExecutor(max_workers=len(jobs)) as pool:
    print(json.dumps(dict(paused_states=states, resumes=list(pool.map(resume, jobs)))))
'''
        addresses = ([f"127.20.{i}.1" for i in range(shards)] if args.isolated else
                     [inspect(job)["NetworkSettings"]["Networks"][project + "_default"]["IPAddress"] for job in jobs])
        write_json(result / "generator-addresses.json", dict(zip(jobs, addresses)))
        synchronized = run("docker", "exec", server, "python3", "-c", sync, json.dumps(addresses), str(vus // shards))
        write_json(result / "synchronization.json", json.loads(synchronized.stdout))
        print("All generators resumed; collecting synchronized ramp and hold.", flush=True)
        exits = []
        for i, job in enumerate(jobs):
            value = int(run("docker", "wait", job).stdout.strip())
            (result / f"shard-{i}/exit.txt").write_text(f"{value}\n")
            exits.append(value)
        code = max(exits)
        (result / "exit.txt").write_text(f"{code}\n")
        print(f"Generator exits: {exits}", flush=True)
    finally:
        stop.set()
        if stats_thread:
            stats_thread.join(timeout=15)
        if monitor:
            run("docker", "exec", server, "sh", "-c",
                'if [ -f "$1" ]; then kill -TERM "$(cat "$1")"; fi', "_", pid_file, check=False)
            try:
                monitor.wait(timeout=10)
            except subprocess.TimeoutExpired:
                monitor.terminate()
        for i, job in enumerate(jobs):
            directory = result / f"shard-{i}"
            if run("docker", "inspect", job, check=False).returncode == 0:
                run("docker", "stop", "--time", "10", job, check=False)
                log = run("docker", "logs", job, check=False)
                (directory / "console.log").write_text(log.stdout + log.stderr)
                write_json(directory / "loadgen.json", [inspect(job)])
                run("docker", "rm", job, check=False)
        write_json(result / "server-after.json", [inspect(server)])
        (result / "health-after.json").write_text(run("docker", "exec", server, "curl", "-fsS", "http://127.0.0.1/health", check=False).stdout)
        log = run("docker", "logs", server, check=False)
        (result / "server-console.log").write_text(log.stdout + log.stderr)
        run("docker", "cp", f"{server}:/var/log/nginx", str(result / "nginx-logs"), check=False)
    print("Local diagnostic only; examine merged samples and generator headroom.", flush=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
