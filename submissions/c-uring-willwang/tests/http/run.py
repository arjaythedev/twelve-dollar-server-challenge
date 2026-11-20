"""One fresh-database, single-server-CPU HTTP trial with four concurrent deterministic client shards."""

import argparse
import array
import hashlib
import json
import os
import pathlib
import re
import resource
import socket
import shutil
import subprocess
import sys
import time
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("server", type=pathlib.Path)
parser.add_argument("tag")
parser.add_argument(
    "--work", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent
)
parser.add_argument("--memory-mib", type=int, default=0)
parser.add_argument("--cpu", type=int, default=10)
parser.add_argument("--load-cpus", default="11,12,13,14")
parser.add_argument("--port", default=19400, type=int)
parser.add_argument("--vus", type=int, default=4096)
parser.add_argument("--ramp", type=int, default=5)
parser.add_argument("--warm", type=int, default=12)
parser.add_argument("--hold", type=int, default=30)
parser.add_argument("--think", type=float, default=0.004)
parser.add_argument("--seed", type=int, default=20261111)
parser.add_argument("--source", type=pathlib.Path, default=pathlib.Path("seed/feed.db"))
parser.add_argument(
    "--tokens", type=pathlib.Path, default=pathlib.Path("seed/tokens.json")
)
args = parser.parse_args()
args.source = args.source.resolve(strict=True)
args.tokens = args.tokens.resolve(strict=True)
args.server = args.server.resolve(strict=True)
assert args.port >= 1024 and args.warm >= args.ramp
root = args.work.resolve()
load_cpus = [int(cpu) for cpu in args.load_cpus.split(",")]
assert args.cpu not in load_cpus and len(set(load_cpus)) == len(load_cpus)
shards = len(load_cpus)
if args.vus > 100000:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        with socket.create_connection(listener.getsockname()), listener.accept()[0]:
            tracker = pathlib.Path("/proc/sys/net/netfilter/nf_conntrack_count")
            limit = pathlib.Path("/proc/sys/net/netfilter/nf_conntrack_max")
            # A dedicated host may have no conntrack or a much larger table.
            # Leave space for other traffic on hosts where tracking is already in use.
            if tracker.exists() and limit.exists():
                used = int(tracker.read_text())
                capacity = int(limit.read_text())
                if used and args.vus > 0.8 * (capacity - used):
                    parser.error(
                        "Local conntrack lacks headroom; run inside a loopback-only network namespace."
                    )
out = root / "runs" / args.tag
out.mkdir(parents=True, exist_ok=False)
dbfile = out / "feed.db"
if not args.memory_mib:
    shutil.copyfile(args.source, dbfile)
clients = []
server = None
server_pid = None
server_cgroup = None


def request_health():
    with urllib.request.urlopen(
        f"http://127.0.0.1:{args.port}/health", timeout=1
    ) as resp:
        return json.load(resp).get("db") == "ok"


def pin_limit():
    os.sched_setaffinity(0, {args.cpu})
    resource.setrlimit(resource.RLIMIT_NOFILE, (65535, 65535))


def tcp_state():
    for line in pathlib.Path("/proc/net/sockstat").read_text().splitlines():
        name, *fields = line.split()
        if name == "TCP:":
            state = {key: int(value) for key, value in zip(fields[::2], fields[1::2])}
            for key in ("tcp_ehash_entries", "tcp_child_ehash_entries"):
                path = pathlib.Path("/proc/sys/net/ipv4") / key
                if path.exists():
                    state[key] = int(path.read_text())
            return state
    return {}


def sample():
    stat = (
        pathlib.Path(f"/proc/{server_pid}/stat").read_text().split(") ", 1)[1].split()
    )
    status = pathlib.Path(f"/proc/{server_pid}/status").read_text()
    rss = int(re.search(r"^VmRSS:\s+(\d+)", status, re.M)[1])
    peak = int(re.search(r"^VmHWM:\s+(\d+)", status, re.M)[1])
    result = {
        "wall": time.monotonic(),
        "cpu": (int(stat[11]) + int(stat[12])) / os.sysconf("SC_CLK_TCK"),
        "rss_kib": rss,
        "peak_kib": peak,
        "tcp_state": tcp_state(),
    }
    if server_cgroup is not None:

        def counters(filename):
            return {
                k: int(v)
                for k, v in (
                    line.split()
                    for line in (server_cgroup / filename).read_text().splitlines()
                )
            }

        result["cpu"] = counters("cpu.stat")["usage_usec"] / 1e6
        mstat = counters("memory.stat")
        result["cgroup"] = {
            "current": int((server_cgroup / "memory.current").read_text()),
            "peak": int((server_cgroup / "memory.peak").read_text()),
            "max": int((server_cgroup / "memory.max").read_text()),
            "swap_max": int((server_cgroup / "memory.swap.max").read_text()),
            "events": counters("memory.events"),
        }
        result["cgroup"].update(
            {k: mstat[k] for k in ("anon", "file", "kernel", "sock", "slab")}
        )
    return result


try:
    before_ramp = tcp_state()
    with (out / "server.log").open("wb") as logfile:
        command = [str(args.server.resolve())]
        env = {
            **os.environ,
            "HOST": "127.0.0.1",
            "PORT": str(args.port),
            "JWT_SECRET": "twelve-dollar-challenge",
            "SQLITE_PATH": str(dbfile),
        }
        pidfile = out / "server.pid"
        if args.memory_mib:
            # Copying within the server's own scope charges even the initial seed file cache.
            env["XDG_RUNTIME_DIR"] = "/run/user/" + str(os.getuid())
            launch = """
import os
import pathlib
import shutil
import sys
source, database, pidfile, executable = sys.argv[1:]
shutil.copyfile(source, database)
pathlib.Path(pidfile).write_text(str(os.getpid()))
os.execv(executable, [executable])
"""
            command = [
                "systemd-run",
                "--user",
                "--scope",
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
                *command,
            ]
        server = subprocess.Popen(
            command,
            cwd=out,
            env=env,
            stdout=logfile,
            stderr=subprocess.STDOUT,
            preexec_fn=pin_limit,
        )
    cutoff = time.monotonic() + 60
    while time.monotonic() < cutoff and server.poll() is None:
        try:
            if request_health():
                break
        except OSError:
            time.sleep(0.1)
    else:
        raise RuntimeError(
            "server did not become healthy: " + (out / "server.log").read_text()
        )
    server_pid = int(pidfile.read_text()) if args.memory_mib else server.pid
    if args.memory_mib:
        rel = (
            pathlib.Path(f"/proc/{server_pid}/cgroup")
            .read_text()
            .strip()
            .split("::")[-1]
        )
        server_cgroup = pathlib.Path("/sys/fs/cgroup") / rel.lstrip("/")
        assert (
            int((server_cgroup / "memory.max").read_text())
            == args.memory_mib * 1024 * 1024
        )
        assert (server_cgroup / "memory.swap.max").read_text().strip() == "0"
    for shard in range(shards):
        offset = args.vus * shard // shards
        shard_vus = args.vus * (shard + 1) // shards - offset
        cmd = [
            "taskset",
            "-c",
            str(load_cpus[shard]),
            str(root / "mload"),
            "--port",
            str(args.port),
            "--vus",
            str(shard_vus),
            "--ramp",
            str(args.ramp),
            "--duration",
            str(args.warm + args.hold),
            "--think-scale",
            str(args.think),
            "--tokens",
            str(args.tokens),
            "--src-ips",
            "32",
            "--report-every",
            str(args.warm + args.hold + 1),
            "--max-errors",
            str(max(10000, args.vus)),
        ]
        with (out / f"client{shard}.log").open("wb") as log:
            proc = subprocess.Popen(
                cmd,
                stdout=log,
                stderr=subprocess.STDOUT,
                env={
                    **os.environ,
                    "MLOAD_OFFSET": str(offset),
                    "MLOAD_SEED": str(args.seed),
                    "MLOAD_WARM": str(args.warm),
                    "MLOAD_HIST": str(out / f"hist{shard}.bin"),
                    "MLOAD_TICKS": str(out / f"client{shard}-seconds.csv"),
                },
            )
            clients.append(proc)
    # Allow a few ms for the clients' token parsing and startup. Count server CPU for the steady interval.
    t0 = time.monotonic() + 0.02
    while time.monotonic() < t0 + args.warm:
        if server.poll() is not None:
            raise RuntimeError("server exited during warmup")
        time.sleep(min(0.5, max(0, t0 + args.warm - time.monotonic())))
    t_start = sample()
    while time.monotonic() < t0 + args.warm + args.hold:
        if server.poll() is not None:
            raise RuntimeError("server exited during hold")
        time.sleep(min(0.5, max(0, t0 + args.warm + args.hold - time.monotonic())))
    t_end = sample()
    for p in clients:
        if p.wait(15) != 0:
            raise RuntimeError(
                "load generator failed: "
                + str([x.read_text()[-300:] for x in out.glob("client*.log")])
            )
    ticks = t_end["cpu"] - t_start["cpu"]
    wall = t_end["wall"] - t_start["wall"]
    hist = [0] * 160000
    count = errors = conns = 0
    tcp = {k: 0 for k in ("start", "end", "peak", "successes", "failed")}
    has_tcp = True
    endpoints = [0] * 4
    names = ("feed", "get_post", "like", "create")
    routes = ("GET /feed", "GET /posts/:id", "POST /posts/:id/like", "POST /posts")
    outstanding = {}
    connecting = 0
    for shard in range(shards):
        contents = (out / f"client{shard}.log").read_text()
        m = re.search(r"total requests=(\d+) errors=(\d+) .*? connects=(\d+)", contents)
        count += int(m[1])
        errors += int(m[2])
        conns += int(m[3])
        line = re.search(
            r"connections_open start=(\d+) end=(\d+) peak=(\d+) successes=(\d+) failed=(\d+)",
            contents,
        )
        if line:
            for key, value in zip(tcp, line.groups()):
                tcp[key] += int(value)
        else:
            has_tcp = False
        pending_connect = re.search(r"^pending_connect=(\d+)", contents, re.M)
        if pending_connect:
            connecting += int(pending_connect[1])
        for ep, active, over, oldest in re.findall(
            r"^pending_http ep=(\d+) count=(\d+) over60s=(\d+) oldest_us=(\d+)", contents, re.M
        ):
            item = outstanding.setdefault(
                routes[int(ep)], {"pending": 0, "aged_at_or_above_60s": 0, "max_age_ms": 0}
            )
            item["pending"] += int(active)
            item["aged_at_or_above_60s"] += int(over)
            item["max_age_ms"] = max(item["max_age_ms"], int(oldest) / 1000)
        for i, m in enumerate(
            re.finditer(r"^(?:GET|POST) /\S+\s+(\d+)\s+(\d+)", contents, re.M)
        ):
            endpoints[i] += int(m[1])
        hits = array.array("I")
        with (out / f"hist{shard}.bin").open("rb") as hf:
            hits.fromfile(hf, 160000)
        for i, n in enumerate(hits):
            hist[i] += n

    def quantile(p):
        threshold = p * count
        n = 0
        for i, h in enumerate(hist):
            n += h
            if n >= threshold:
                return round(i * 0.01 if i < 100000 else i - 100000, 2)
        return 60000

    result = {
        "server": str(args.server),
        "server_sha256": hashlib.sha256(args.server.read_bytes()).hexdigest(),
        "generator_sha256": hashlib.sha256((root / "mload").read_bytes()).hexdigest(),
        "tag": args.tag,
        "vus": args.vus,
        "think_scale": args.think,
        "ramp_s": args.ramp,
        "warm_s": args.warm,
        "hold_s": args.hold,
        "seed": args.seed,
        "requests": count,
        "endpoints": dict(zip(names, endpoints)),
        "rps": round(count / args.hold, 1),
        "p50_ms": quantile(0.5),
        "p95_ms": quantile(0.95),
        "p99_ms": quantile(0.99),
        "errors": errors,
        "completed_at_or_above_500ms": count - sum(hist[:50000]),
        "completed_at_or_above_1s": count - sum(hist[:100000]),
        "completed_at_or_above_60s": count - sum(hist),
        "outstanding_at_stop": {"connecting": connecting, "endpoints": outstanding} if outstanding else None,
        "connections": conns,
        "tcp": tcp if has_tcp else None,
        "net_stats": {
            "before_server": before_ramp,
            "hold_start": t_start["tcp_state"],
            "hold_end": t_end["tcp_state"],
        },
        "cpu_s": round(ticks, 3),
        "cpu_pct": round(ticks * 100 / wall, 2),
        "cpu_us_per_req": round(ticks * 1e6 / count, 3),
        "rss_kib_end": t_end["rss_kib"],
        "rss_kib_peak": t_end["peak_kib"],
        "cgroup": t_end.get("cgroup"),
    }
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
finally:
    for p in clients:
        if p.poll() is None:
            p.terminate()
    for p in clients:
        try:
            p.wait(3)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    if server and server.poll() is None:
        if server_pid and server_pid != server.pid:
            try:
                os.kill(server_pid, 15)
            except ProcessLookupError:
                pass
        server.terminate()
        try:
            server.wait(5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
    # The source database is never served. Only delete private trial copies.
    for path in [
        dbfile,
        pathlib.Path(str(dbfile) + "-wal"),
        pathlib.Path(str(dbfile) + "-shm"),
    ]:
        path.unlink(missing_ok=True)
