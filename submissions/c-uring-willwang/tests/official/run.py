"""Run independent shards of the unmodified organizer mload using local private target aliases."""

import argparse
import hashlib
import http.client
import ipaddress
import json
import math
import os
from pathlib import Path
import resource
import select
import signal
import socket
import subprocess
import sys
import time
import uuid

NET = Path("/proc/sys/net")
RECORDED_BUILD = "48df1cd6242f55ee5be98238a54fbf7861fcc83c6e3cc91d334b7903d4f208e6"


def challenge_root():
    for parent in Path(__file__).resolve().parents:
        if (parent / "seed/tokens.json").is_file():
            return parent
    return Path.cwd()


def sha(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def fields(path):
    return {key: int(value) for key, value in (line.split() for line in path.read_text().splitlines())}


def parse_reserved():
    reserved = set()
    for spec in (NET / "ipv4/ip_local_reserved_ports").read_text().strip().split(","):
        if not spec:
            continue
        left, _, right = spec.partition("-")
        reserved.update(range(int(left), int(right or left) + 1))
    return reserved


def tcp_counters():
    values = {}
    groups = (
        (
            "snmp",
            "Tcp",
            (
                "ActiveOpens",
                "PassiveOpens",
                "CurrEstab",
                "AttemptFails",
                "EstabResets",
                "RetransSegs",
                "InSegs",
                "OutSegs",
            ),
        ),
        (
            "netstat",
            "TcpExt",
            ("ListenOverflows", "ListenDrops", "TCPOrigDataSent", "DelayedACKs", "TCPTimeouts", "TCPKeepAlive"),
        ),
    )
    for name, section, desired in groups:
        keys, numbers = [
            s.split()[1:]
            for s in (Path("/proc/net") / name).read_text().splitlines()
            if s.startswith(section + ":")
        ]
        values.update({k: int(v) for k, v in zip(keys, numbers) if k in desired})
    return values


def memory_available():
    for line in Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemAvailable:"):
            return int(line.split()[1]) * 1024
    raise RuntimeError("MemAvailable missing")


def process_sample(pid):
    try:
        values = Path(f"/proc/{pid}/stat").read_text().split(") ", 1)[1].split()
        rss = int(values[21]) * os.sysconf("SC_PAGE_SIZE")
        return {"pid": pid, "rss": rss, "cpu_ticks": int(values[11]) + int(values[12])}
    except (FileNotFoundError, ProcessLookupError):
        return {}


def host_core_counters(core_ids):
    """Counters include other host tasks on these CPUs, even inside a private network namespace."""
    labels = ("user", "nice", "system", "idle", "iowait", "irq", "softirq", "steal")
    cores = {}
    for line in Path("/proc/stat").read_text().splitlines():
        name, *values = line.split()
        if name.startswith("cpu") and name[3:].isdigit() and int(name[3:]) in core_ids:
            cores[name[3:]] = dict(zip(labels, map(int, values[: len(labels)])))
    rows = Path("/proc/softirqs").read_text().splitlines()
    columns = {name[3:]: i for i, name in enumerate(rows[0].split())}
    for row in rows[1:]:
        name, *values = row.split()
        if name in ("NET_RX:", "NET_TX:"):
            for core, counts in cores.items():
                counts[name[:-1].lower() + "_events"] = int(values[columns[core]])
    return cores


def listen_health(port):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=0.5)
    try:
        conn.request("GET", "/health")
        response = conn.getresponse()
        body = json.loads(response.read())
        return response.status == 200 and isinstance(body, dict) and body.get("db") == "ok"
    except (OSError, http.client.HTTPException, ValueError):
        return False
    finally:
        conn.close()


def verify_close(pidfd, scope, port, cutoff):
    poller = select.poll()
    poller.register(pidfd, select.POLLIN)
    until = time.monotonic() + 120
    connections = tcp_counters()["CurrEstab"]
    while connections and not poller.poll(0) and time.monotonic() < until:
        time.sleep(0.1)
        connections = tcp_counters()["CurrEstab"]
    drained_at = time.monotonic() - cutoff
    healthy = not connections and not poller.poll(0) and listen_health(port)
    try:
        events = fields(scope / "memory.events")
        peak = int((scope / "memory.peak").read_text())
    except FileNotFoundError:
        events, peak = None, None
    alive = not poller.poll(0)
    return {
        "accepted_connections_remaining_before_health": connections,
        "seconds_after_scored_cutoff": drained_at,
        "original_process_alive": alive,
        "database_health_ok": bool(healthy),
        "memory_events": events,
        "lifetime_cgroup_peak_bytes": peak,
        "passed": bool(healthy and alive and events is not None and events["oom_kill"] == 0),
    }


def wait_until(when, check):
    while True:
        left = when - time.monotonic()
        if left <= 0:
            return
        check()
        time.sleep(min(left, 0.3))


def cpus(value):
    parts = [set(map(int, group.split(","))) for group in value.split(";")]
    flat = [n for part in parts for n in part]
    if len(flat) != len(set(flat)) or not flat:
        raise ValueError("client groups must be nonempty and disjoint")
    return parts


def plan(args):
    if os.readlink("/proc/self/ns/net") == os.environ.get("ORIGINAL_NET_NS") or not os.environ.get(
        "ORIGINAL_NET_NS"
    ):
        raise RuntimeError("use score-private.sh to create a private namespace")
    if socket.if_nameindex() != [(1, "lo")]:
        raise RuntimeError("requires a private loopback-only namespace")
    if not args.work.is_dir() or not args.program.is_file():
        raise RuntimeError("run build-client.sh with the selected working directory first")
    checksum = sha(args.program)
    if checksum != (args.work / "mload.sha256").read_text().strip():
        raise RuntimeError("mload checksum differs from the build manifest")
    if not args.server.is_file() or not args.source.is_file() or not args.tokens.is_file():
        raise RuntimeError("server, seed and tokens must exist")
    if args.users < 1 or args.ramp < 1 or args.hold < 1 or not 1024 <= args.port <= 65535:
        raise ValueError("positive users and durations, nonprivileged valid port required")
    if not 0 < args.port_occupancy <= 1:
        raise ValueError("port occupancy must be in (0, 1]")
    if len(args.groups) > args.users or len(args.groups) > 254 or args.users > 500000:
        raise ValueError("user/shard combination outside this local harness")
    allowed = os.sched_getaffinity(0)
    client_set = set.union(*args.groups)
    if args.server_cpu in client_set or {args.server_cpu, *client_set} - allowed:
        raise ValueError("CPUs must be available; server and clients must be disjoint")
    low, high = map(int, (NET / "ipv4/ip_local_port_range").read_text().split())
    reserved = parse_reserved() | {args.port}
    width = high - low + 1 - sum(low <= x <= high for x in reserved)
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    shards = []
    for shard, group in enumerate(args.groups):
        start, end = args.users * shard // len(args.groups), args.users * (shard + 1) // len(args.groups)
        count = end - start
        # Distinct target tuples substitute for source hosts the stock binary cannot bind explicitly.
        # Connect chooses a preferred port parity first. Keep ample headroom in both
        # parities so scanning for a free source port does not dominate the client ramp.
        naddr = max(1, math.ceil(count / (width * args.port_occupancy)))
        if naddr > 253 or hard != resource.RLIM_INFINITY and hard < count + 128:
            raise RuntimeError("insufficient target aliases or inherited client hard FD limit")
        aliases = [str(ipaddress.IPv4Address(f"127.81.{shard + 1}.{j}")) for j in range(1, naddr + 1)]
        shards.append(
            {
                "cpus": sorted(group),
                "offset": start,
                "users": count,
                "targets": aliases,
                "available_tuples": naddr * width,
            }
        )
    # The three persistent byte arrays alone can retain 4096+1024+5102 bytes/VU
    # after a seed-sized feed. 32 KiB/VU is only a capacity planning allowance,
    # not an asserted live RSS; includes stack, heap, GC and socket headroom.
    estimated = args.users * 32768 + (args.memory_mib + 1024) * 1024 * 1024
    current_free = memory_available()
    if current_free < estimated:
        raise RuntimeError(f"host memory headroom {current_free} is below planning allowance {estimated}")
    out = dict(
        generator_sha256=checksum,
        matches_recorded_generator=checksum == RECORDED_BUILD,
        server_sha256=sha(args.server),
        seed_bytes=args.source.stat().st_size,
        seed_sha256=sha(args.source),
        tokens_sha256=sha(args.tokens),
        generator_commit="ae4e5a74b399d41851b2f188c91122aaa6d9b7a0",
        generator_build=(args.work / "mload.build.txt").read_text().strip(),
        kernel_release=os.uname().release,
        server=str(args.server.resolve()),
        topology="private loopback only",
        mtu=int(Path("/sys/class/net/lo/mtu").read_text()),
        port=args.port,
        users=args.users,
        ramp_s=args.ramp,
        hold_s=args.hold,
        think_scale=1,
        gzip=False,
        timeout_s=60,
        server_cpu=args.server_cpu,
        server_memory_mib=args.memory_mib,
        server_nofile=65535,
        cpu_tick_hz=os.sysconf("SC_CLK_TCK"),
        client_fd_limit=[soft, hard],
        ephemeral=[low, high],
        max_port_occupancy=args.port_occupancy,
        mem_available_before=current_free,
        working_allowance_bytes=estimated,
        groups=shards,
        tcp_ehash_entries=(NET / "ipv4/tcp_ehash_entries").read_text().strip(),
    )
    return out


def run(args, info):
    if not args.tag or args.tag in {".", ".."} or Path(args.tag).name != args.tag:
        raise ValueError("pass a new single-component tag")
    results = args.work / "runs" / args.tag
    results.mkdir(parents=True, exist_ok=False)
    (results / "plan.json").write_text(json.dumps(info, indent=2))
    server_path = args.server.resolve()
    pidfile = results / "server.pid"
    dbfile = results / "feed.db"
    server = None
    clients = []
    server_pid = None
    server_pidfd = None
    scope = None
    unit = "twelve-score-" + uuid.uuid4().hex + ".scope"
    measured_cores = set.union(*args.groups) | {args.server_cpu}
    extra = os.sched_getaffinity(0) - measured_cores
    if extra:
        os.sched_setaffinity(0, extra)

    def launch_core():
        os.sched_setaffinity(0, {args.server_cpu})
        resource.setrlimit(resource.RLIMIT_NOFILE, (65535, 65535))

    def sample():
        cgcpu = fields(scope / "cpu.stat")
        memory = {
            k: int((scope / k).read_text()) for k in ("memory.current", "memory.peak", "memory.swap.current")
        }
        memory.update(
            {
                k: v
                for k, v in fields(scope / "memory.stat").items()
                if k in ("anon", "file", "kernel", "sock", "slab")
            }
        )
        return dict(
            wall=time.time(),
            mono=time.monotonic(),
            server_cpu=cgcpu,
            server_mem=memory,
            server_pid=process_sample(server_pid),
            clients=[process_sample(p.pid) for p in clients],
            host_core_counters=host_core_counters(measured_cores),
            mem_available=memory_available(),
            tcp=tcp_counters(),
        )

    def alive():
        if server.poll() is not None:
            raise RuntimeError("server exited: " + (results / "server.log").read_text()[-300:])
        for p in clients:
            rc = p.poll()
            if rc not in (None, 0):
                raise RuntimeError(f"generator {p.pid} exited {rc}")

    try:
        launch = (
            "import os,pathlib,shutil,sys; shutil.copyfile(sys.argv[1],sys.argv[2]); "
            "pathlib.Path(sys.argv[3]).write_text(str(os.getpid())); os.execv(sys.argv[4],[sys.argv[4]])"
        )
        cmd = [
            "systemd-run",
            "--user",
            "--scope",
            "--unit",
            unit,
            "--quiet",
            "-p",
            "MemoryAccounting=yes",
            "-p",
            f"MemoryMax={args.memory_mib}M",
            "-p",
            "MemorySwapMax=0",
            sys.executable,
            "-c",
            launch,
            str(args.source),
            str(dbfile),
            str(pidfile),
            str(server_path),
        ]
        env = dict(
            os.environ,
            HOST="0.0.0.0",
            PORT=str(args.port),
            SQLITE_PATH=str(dbfile),
            JWT_SECRET="twelve-dollar-challenge",
            XDG_RUNTIME_DIR=f"/run/user/{os.getuid()}",
        )
        with (results / "server.log").open("wb") as log:
            server = subprocess.Popen(
                cmd, cwd=results, stdout=log, stderr=subprocess.STDOUT, env=env, preexec_fn=launch_core
            )
        deadline = time.monotonic() + 60
        while not listen_health(args.port):
            alive()
            if time.monotonic() > deadline:
                raise RuntimeError("server health timed out")
            time.sleep(0.05)
        server_pid = int(pidfile.read_text())
        server_pidfd = os.pidfd_open(server_pid)
        group = Path(f"/proc/{server_pid}/cgroup").read_text().strip().split("::")[-1]
        scope = Path("/sys/fs/cgroup") / group.lstrip("/")
        if (
            int((scope / "memory.max").read_text()) != args.memory_mib * 1024**2
            or int((scope / "memory.swap.max").read_text()) != 0
        ):
            raise RuntimeError("unexpected service cgroup limits")
        if os.sched_getaffinity(server_pid) != {args.server_cpu}:
            raise RuntimeError("server affinity differs")
        tracker = NET / "netfilter/nf_conntrack_count"
        if tracker.exists() and int(tracker.read_text()) != 0:
            raise RuntimeError("private namespace tracks connections")
        start_ms = math.ceil((time.time() + 3.5) * 1000)
        start_mono = time.monotonic() + start_ms / 1000 - time.time()
        jsonpaths = []
        for idx, desc in enumerate(info["groups"]):
            result_path = results / f"shard{idx}.json"
            jsonpaths.append(result_path)
            cmd = [
                str(args.program),
                "-target",
                ",".join(desc["targets"]),
                "-ports",
                str(args.port),
                "-users",
                str(desc["users"]),
                "-offset",
                str(desc["offset"]),
                "-start",
                str(start_ms),
                "-ramp",
                f"{args.ramp}s",
                "-hold",
                f"{args.hold}s",
                "-think-scale",
                "1",
                "-timeout",
                "60s",
                "-tokens",
                str(args.tokens),
                "-out",
                str(result_path),
            ]

            def pin_client(spec=desc):
                os.sched_setaffinity(0, spec["cpus"])
                soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
                desired = max(soft, spec["users"] + 128) if soft != resource.RLIM_INFINITY else soft
                resource.setrlimit(resource.RLIMIT_NOFILE, (desired, hard))

            with (results / f"shard{idx}.log").open("wb") as log:
                p = subprocess.Popen(
                    cmd,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    env=dict(os.environ, GOMAXPROCS=str(len(desc["cpus"]))),
                    preexec_fn=pin_client,
                )
            clients.append(p)
        (results / "runtime.json").write_text(
            json.dumps({"start_unix_ms": start_ms, "scope": str(scope), "pid": server_pid}, indent=2)
        )
        duration = args.ramp + args.hold
        points = sorted(set([0, args.ramp, max(0, duration - 0.25), duration, *range(5, duration, 5)]))
        samples = []
        with (results / "samples.jsonl").open("w") as f:
            for t in points:
                wait_until(start_mono + t, alive)
                s = sample()
                s["scheduled_s"] = t
                samples.append(s)
                print(json.dumps(s), file=f, flush=True)
        for p in clients:
            if p.wait(timeout=20) != 0:
                raise RuntimeError("generator exited unsuccessfully")
        with (results / "merge.log").open("w") as f:
            subprocess.run(
                [str(args.program), "-merge", "-out", str(results / "score.json"), *map(str, jsonpaths)],
                check=True,
                stdout=f,
                stderr=subprocess.STDOUT,
            )
        merged = json.loads((results / "score.json").read_text())
        reports = [json.loads(p.read_text()) for p in jsonpaths]
        first, last = samples[0], samples[-1]
        hold_start = next(x for x in samples if x["scheduled_s"] == args.ramp)

        def cpu(a, b):
            wall = b["mono"] - a["mono"]
            use = b["server_cpu"]["usage_usec"] - a["server_cpu"]["usage_usec"]
            return {"wall_s": wall, "use_usec": use, "mean_cpu_pct": use / (wall * 1e4)}

        def core_deltas(a, b):
            before, after = a["host_core_counters"], b["host_core_counters"]
            return {
                core: {k: value - before[core][k] for k, value in counts.items()}
                for core, counts in after.items()
            }

        complete_cpu = cpu(first, last)
        complete_cpu["cpu_us_per_req"] = complete_cpu["use_usec"] / merged["reqs"] if merged["reqs"] else None
        # CurrEstab is both TCP endpoints in this private namespace, including
        # CLOSE_WAIT per RFC. Stock mload also supplies client-only open sockets.
        by_t = {}
        for i, r in enumerate(reports):
            for tick in r.get("timeline", []):
                by_t.setdefault(tick["t"], {})[i] = tick["conns"]
        peak = max([sum(t.values()) for t in by_t.values() if len(t) == len(reports)] or [0])
        client_metrics = []
        for idx, p in enumerate(clients):
            points = [s for s in samples if s["clients"][idx].get("rss", 0) > 0]
            measured = next((x for x in reversed(points) if x["scheduled_s"] < duration), None)
            one = {"pid": p.pid, "peak_sampled_rss": max([s["clients"][idx]["rss"] for s in points] or [0])}
            if measured and first["clients"][idx].get("cpu_ticks") is not None:
                dt = measured["mono"] - first["mono"]
                if dt > 0:
                    one["mean_cpus_used"] = (
                        (measured["clients"][idx]["cpu_ticks"] - first["clients"][idx]["cpu_ticks"])
                        / info["cpu_tick_hz"]
                        / dt
                    )
            client_metrics.append(one)
        summary = {
            k: merged[k]
            for k in (
                "users",
                "duration_s",
                "reqs",
                "req_per_s",
                "fails",
                "p95_ms",
                "p99_ms",
                "err_pct",
                "pass",
                "dials",
                "errors",
            )
        }
        summary.update(
            whole_run_server_cpu=complete_cpu,
            hold_server_cpu=cpu(hold_start, last),
            host_core_tick_hz=info["cpu_tick_hz"],
            whole_run_host_core_deltas=core_deltas(first, last),
            hold_host_core_deltas=core_deltas(hold_start, last),
            server_cgroup_peak_bytes=last["server_mem"]["memory.peak"],
            max_open_clients_5s=peak,
            max_established_pairs_sampled=max(s["tcp"]["CurrEstab"] for s in samples) / 2,
            private_tcp_deltas={
                k: last["tcp"][k] - first["tcp"][k] for k in first["tcp"] if k != "CurrEstab"
            },
            clients=client_metrics,
        )
        (results / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, indent=2))
        if args.verify_close:
            closure = verify_close(server_pidfd, scope, args.port, start_mono + duration)
            (results / "close.json").write_text(json.dumps(closure, indent=2) + "\n")
            if not closure["passed"]:
                raise RuntimeError("post-score disconnect or database health check failed. See close.json")
    finally:
        for p in clients:
            if p.poll() is None:
                p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        if server_pidfd is not None:
            try:
                signal.pidfd_send_signal(server_pidfd, signal.SIGTERM)
            except ProcessLookupError:
                pass
            os.close(server_pidfd)
        if server:
            if server.poll() is None:
                # This includes startup and any checkpoint subprocess in our own scope.
                subprocess.run(
                    ["systemctl", "--user", "stop", unit],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=10,
                    check=False,
                )
                if server.poll() is None and not server_pid:
                    server.terminate()
            try:
                server.wait(5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
        for f in [dbfile, Path(str(dbfile) + "-wal"), Path(str(dbfile) + "-shm")]:
            f.unlink(missing_ok=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("tag", nargs="?")
    parser.add_argument("--users", type=int, required=True)
    parser.add_argument("--ramp", type=int, default=60)
    parser.add_argument("--hold", type=int, default=300)
    parser.add_argument("--port", type=int, default=19432)
    parser.add_argument("--server-cpu", type=int, default=10)
    parser.add_argument(
        "--client-groups", dest="groups", type=cpus, default=cpus("0,1,2;3,4,5;6,7,8;11,12,13")
    )
    parser.add_argument("--memory-mib", type=int, default=1750)
    parser.add_argument(
        "--port-occupancy",
        type=float,
        default=0.15,
        help="upper target-pair occupancy as a fraction of the available ephemeral ports",
    )
    parser.add_argument("--source", type=Path, default=challenge_root() / "seed/feed.db")
    parser.add_argument("--tokens", type=Path, default=challenge_root() / "seed/tokens.json")
    parser.add_argument("--preflight", action="store_true")
    parser.add_argument("--verify-close", action="store_true", help="after scoring, wait for all sockets to drain and check the original server's database health")
    args = parser.parse_args()
    args.work = args.work.resolve()
    args.program = args.work / "mload"
    args.source = args.source.resolve()
    args.tokens = args.tokens.resolve()
    conf = plan(args)
    if args.preflight:
        print(json.dumps(conf, indent=2))
    else:
        run(args, conf)
