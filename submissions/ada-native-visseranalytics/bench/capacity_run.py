#!/usr/bin/env python3
"""Run isolated low-memory diagnostic shards. Never report this as a k6 score."""
import argparse
import array
import concurrent.futures
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import threading
import time

ADA = Path(__file__).resolve().parents[1]
ROOT = ADA.parents[1]


def run(*command, check=True):
    result = subprocess.run(command, text=True, capture_output=True)
    if check and result.returncode:
        raise RuntimeError(f"{command[:3]}: {result.stderr}")
    return result


def save(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n")


def inspect(name):
    return json.loads(run("docker", "inspect", name).stdout)[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project")
    parser.add_argument("--vus", type=int, required=True)
    parser.add_argument("--shards", type=int, default=8)
    parser.add_argument("--ramp", type=float, default=60)
    parser.add_argument("--hold", type=float, default=120)
    parser.add_argument("--memory-gb", type=int, default=2)
    parser.add_argument("--cpus", type=float, default=1)
    parser.add_argument("--isolated", action="store_true", help="share a network_mode:none server namespace")
    args = parser.parse_args()
    assert 1 <= args.shards <= 16 and args.vus > 0 and args.vus % args.shards == 0
    assert args.vus // args.shards < 60000 and args.ramp > 0 and args.hold > 0
    vm = json.loads(run("docker", "info", "--format", "{{json .}}").stdout)
    assert (args.shards * args.memory_gb + 3) * 1024**3 < vm["MemTotal"]
    server = args.project + "-server-1"
    before = inspect(server)
    assert before["State"]["Running"]
    run("docker", "exec", server, "curl", "-fsS", "http://127.0.0.1/health")
    manifest = run("docker", "exec", server, "cat", "/app/build-sha256.txt").stdout
    built = {line.split(None, 1)[1]: line.split(None, 1)[0] for line in manifest.splitlines()}
    source = sorted((ADA / "src").glob("*")) + [ADA / "build.sh", ADA / "versions.env"]
    assert built == {str(p.relative_to(ADA)): hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in source}, "running binary differs from source"
    ident = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + f"-vu{args.vus}-{os.getpid()}"
    result = ADA / "benchmark-results/diagnostic" / ident
    result.mkdir(parents=True)
    files = source + [ADA / "bench/capacity_client.py", Path(__file__).resolve(), ROOT / "bench/load.js",
                      ADA / "compose.isolated.yaml"]
    save(result / "sha256.json", {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                  for p in files})
    inputs = {}
    for path in (ROOT / "seed/feed.db", ROOT / "seed/tokens.json"):
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        inputs[str(path.relative_to(ROOT))] = digest.hexdigest()
    save(result / "inputs-sha256.json", inputs)
    for path in files:
        target = result / "source" / path.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
    (result / "build-sha256.txt").write_text(manifest)
    save(result / "server-before.json", before)
    save(result / "server-image.json", json.loads(run("docker", "image", "inspect", before["Image"]).stdout))
    probe = """
import json, platform
from pathlib import Path
pid = int(Path('/run/ada/app.pid').read_text())
p = Path('/proc') / str(pid)
print(json.dumps(dict(architecture=platform.machine(),
    emulator_mappings=[line for line in (p/'maps').read_text().splitlines()
                       if 'rosetta' in line.lower() or 'qemu' in line.lower()],
    limits=[line for line in (p/'limits').read_text().splitlines() if 'Max open files' in line],
    cgroup={name: (Path('/sys/fs/cgroup')/name).read_text().strip()
            for name in ('cpu.max','memory.max','memory.swap.max')})))
"""
    save(result / "server-runtime.json", json.loads(run("docker", "exec", "--user", "10001:10001",
         server, "python3", "-c", probe).stdout))
    save(result / "docker-vm.json", {k: vm[k] for k in ("MemTotal", "NCPU", "Architecture")})
    jobs = [f"ada-diag-{ident}-{i}" for i in range(args.shards)]
    start_at = time.time() + 20
    save(result / "plan.json", vars(args) | dict(start_at=start_at, jobs=jobs, diagnostic_only=True))
    platform = "linux/arm64" if vm["Architecture"] in ("aarch64", "arm64") else "linux/amd64"
    stop = threading.Event()
    stats = None

    def interrupt(signum, frame):
        raise InterruptedError(signum)

    signal.signal(signal.SIGTERM, interrupt)
    signal.signal(signal.SIGINT, interrupt)

    def launch(index):
        child = result / f"shard-{index}"
        child.mkdir()
        count = args.vus // args.shards
        run("docker", "run", "--detach", "--name", jobs[index], "--platform", platform,
            "--network", "container:" + server if args.isolated else args.project + "_default",
            "--cpus", str(args.cpus),
            "--memory", f"{args.memory_gb}g", "--memory-swap", f"{args.memory_gb}g",
            "--ulimit", "nofile=65535:65535",
            *([] if args.isolated else ["--sysctl", "net.ipv4.ip_local_port_range=1024 65535"]),
            "--mount", f"type=bind,source={ADA / 'bench/capacity_client.py'},target=/client.py,readonly",
            "--mount", f"type=bind,source={ROOT / 'seed/tokens.json'},target=/seed/tokens.json,readonly",
            "--mount", f"type=bind,source={child},target=/results",
            "python:3.14-slim-trixie", "python3", "/client.py", "--vus", str(count),
            "--offset", str(index * count), "--start-at", str(start_at),
            "--ramp", str(args.ramp), "--hold", str(args.hold), "--output", "/results",
            *(["--host", "127.0.0.1", "--local-ips", f"127.10.{index}.2,127.10.{index}.3"]
              if args.isolated else []))

    def sample():
        with (result / "stats.jsonl").open("w") as out:
            while not stop.is_set():
                sampled_at = dt.datetime.now(dt.timezone.utc).isoformat()
                proc = run("docker", "stats", "--no-stream", "--format", "{{json .}}", server, *jobs, check=False)
                for line in proc.stdout.splitlines():
                    try:
                        out.write(json.dumps(dict(sampled_at=sampled_at, stats=json.loads(line))) + "\n")
                    except ValueError:
                        pass
                out.flush()
                stop.wait(3)

    print(f"Diagnostic only. Results: {result}", flush=True)
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.shards) as pool:
            list(pool.map(launch, range(args.shards)))
        stats = threading.Thread(target=sample)
        stats.start()
        print("Shards ready; synchronized ramp and hold scheduled.", flush=True)
        for index, job in enumerate(jobs):
            (result / f"shard-{index}/exit.txt").write_text(run("docker", "wait", job).stdout)
    finally:
        stop.set()
        if stats:
            stats.join(timeout=15)
        for index, job in enumerate(jobs):
            if run("docker", "inspect", job, check=False).returncode == 0:
                run("docker", "stop", "--time", "5", job, check=False)
                child = result / f"shard-{index}"
                save(child / "container.json", inspect(job))
                log = run("docker", "logs", job, check=False)
                (child / "console.log").write_text(log.stdout + log.stderr)
                run("docker", "rm", job, check=False)
        save(result / "server-after.json", inspect(server))
        (result / "health-after.json").write_text(run("docker", "exec", server, "curl", "-fsS",
                                                     "http://127.0.0.1/health", check=False).stdout)
        cgroup_probe = """
import json
from pathlib import Path
print(json.dumps({name: (Path('/sys/fs/cgroup')/name).read_text().strip()
    for name in ('memory.current','memory.peak','memory.max','memory.events',
                 'memory.swap.max','cpu.max','cpu.stat')}))
"""
        telemetry = run("docker", "exec", server, "python3", "-c", cgroup_probe, check=False)
        if telemetry.returncode == 0:
            save(result / "server-cgroup-after.json", json.loads(telemetry.stdout))
        log = run("docker", "logs", server, check=False)
        (result / "server-console.log").write_text(log.stdout + log.stderr)
    summaries = [json.loads((result / f"shard-{i}/summary.json").read_text()) for i in range(args.shards)]
    samples = array.array("d")
    for i in range(args.shards):
        samples.frombytes((result / f"shard-{i}/hold-latencies.f64").read_bytes())
    ordered = sorted(samples)
    if not ordered:
        raise RuntimeError("No completed hold samples; diagnostic run is invalid")
    def quantile(q):
        p = (len(ordered) - 1) * q
        lo = int(p)
        return ordered[lo] + (ordered[min(lo + 1, len(ordered) - 1)] - ordered[lo]) * (p - lo)
    failed = sum(s["counts"]["hold_failed"] for s in summaries)
    whole_requests = sum(s["counts"]["requests"] for s in summaries)
    whole_failed = sum(s["counts"]["failed"] for s in summaries)
    try:
        health = json.loads((result / "health-after.json").read_text())
    except ValueError:
        health = {}
    client_states = [json.loads((result / f"shard-{i}/container.json").read_text())["State"]
                     for i in range(args.shards)]
    analysis = dict(diagnostic_only=True, target_vus=args.vus, hold_seconds=args.hold,
                    hold_requests=len(samples), hold_rps=len(samples) / args.hold,
                    whole_requests=whole_requests, whole_failed=whole_failed,
                    whole_failure_rate=whole_failed / whole_requests,
                    healthy_after=health.get("status") == "ok" and health.get("db") == "ok",
                    failure_rate=failed / len(samples), p95_ms=quantile(.95), p99_ms=quantile(.99),
                    hold_check_failures=sum(s["counts"].get("hold_check_failures", 0) for s in summaries),
                    clients_completed=all(s["ExitCode"] == 0 and not s["OOMKilled"] for s in client_states),
                    peak_connected_sum=sum(s["peak_connected"] for s in summaries),
                    minimum_active_hold_sum=sum(s["counts"]["minimum_active_hold"] for s in summaries),
                    max_client_lag_seconds=max(s["client"]["scheduling_lag_max_seconds"] for s in summaries),
                    shards=summaries)
    analysis["diagnostic_thresholds_pass"] = (analysis["p95_ms"] < 500 and analysis["p99_ms"] < 1000 and
        analysis["failure_rate"] < .01 and analysis["minimum_active_hold_sum"] == args.vus and
        analysis["hold_check_failures"] == 0 and analysis["clients_completed"] and
        analysis["whole_failure_rate"] < .01 and analysis["healthy_after"] and
        analysis["max_client_lag_seconds"] < .5 and
        all(not s["unexpected_errors"] and not s["counts"].get("semantic_failures", 0) for s in summaries))
    save(result / "analysis.json", analysis)
    print(json.dumps({k: v for k, v in analysis.items() if k != "shards"}, indent=2), flush=True)
    return 0 if analysis["diagnostic_thresholds_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
