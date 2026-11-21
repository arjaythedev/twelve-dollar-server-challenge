"""Matched stock-mload trials with one private Linux server, router and four generator namespaces."""

import argparse
import datetime
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import pwd
import re
import signal
import subprocess
import sys
import time
import tempfile
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure import host_tcp_counters, sample_socket_queues, snapshot
from assess import assess

HERE = Path(__file__).resolve().parent


def challenge_root():
    for directory in (Path.cwd(), *Path.cwd().parents, HERE, *HERE.parents):
        if (directory / "bench/load.js").is_file() and (directory / "seed/make-seed.sh").is_file():
            return directory
    root = os.environ.get("TWELVE_CHALLENGE_ROOT")
    return Path(root).resolve() if root else HERE.parent


ROOT = challenge_root()
MLOAD = Path(os.environ.get("TWELVE_MLOAD") or (HERE.parent / "mload"))
MLOAD_SHA = "48df1cd6242f55ee5be98238a54fbf7861fcc83c6e3cc91d334b7903d4f208e6"
GROUPS = ("0,1", "3,4", "6,7", "11,12,13")
ROUTER_CPUS = (2, 5, 8, 14)
APP_CPU, MANAGER_CPU = 10, 15


def sha(path):
    with path.open("rb") as fp:
        return hashlib.file_digest(fp, "sha256").hexdigest()


def cpu_mask(mask):
    return ",".join(
        f"{mask >> shift & 0xffffffff:08x}" for shift in reversed(range(0, mask.bit_length(), 32))
    )


def write(path, data):
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def run(*args, check=True, **kw):
    result = subprocess.run([str(x) for x in args], capture_output=True, text=True, **kw)
    if check and result.returncode:
        raise RuntimeError(f"{args} exited {result.returncode}: {result.stderr} {result.stdout}")
    return result.stdout.strip()


def priv(*args, **kw):
    return run("sudo", "-n", *args, **kw)


def ns(name, *args, **kw):
    return priv("ip", "netns", "exec", name, *args, **kw)


def ip(name, *args):
    return priv("ip", "-n", name, *args)


def tcp_counts(app_cpu, router_cpus, manager_cpu):
    keys = ("user", "nice", "sys", "idle", "iowait", "irq", "softirq", "steal")
    selected = {f"cpu{n}" for n in (app_cpu, *router_cpus, manager_cpu)}
    return {
        line.split()[0]: dict(zip(keys, map(int, line.split()[1:])))
        for line in Path("/proc/stat").read_text().splitlines()
        if line.split()[0] in selected
    }


def memstats(cg):
    result = {}
    for filename in ("cpu.stat", "memory.events", "memory.stat"):
        p = cg / filename
        if p.is_file():
            result[filename] = {
                k: int(v)
                for k, v in (l.split() for l in p.read_text().splitlines())
                if filename != "memory.stat"
                or k in ("anon", "file", "sock", "kernel", "slab", "file_dirty", "file_writeback")
            }
    for filename in ("memory.current", "memory.peak", "memory.swap.current"):
        p = cg / filename
        if p.is_file():
            result[filename] = int(p.read_text())
    return result


def set_up(args, names, created, server_rx_masks=None):
    server, router, *clients = names
    expected_rx = (1 << args.app_cpu,) * len(clients) if server_rx_masks is None else tuple(server_rx_masks)
    if len(expected_rx) != len(clients) or any(mask < 0 for mask in expected_rx):
        raise ValueError("provide one nonnegative server RX mask for each routed lane")
    masks = [
        1 << args.app_cpu,
        sum(1 << n for n in set(args.router_cpus)),
        *[sum(1 << int(n) for n in group.split(",")) for group in args.groups],
    ]
    # The transient router is the parent used to allocate a 2 GiB guest-sized
    # TCP established hash for the server alone. Never change host sysctls.
    priv("ip", "netns", "add", router)
    created.append(router)
    ns(router, "sysctl", "-q", "-w", f"net.ipv4.tcp_child_ehash_entries={args.server_ehash}")
    priv("nsenter", f"--net=/run/netns/{router}", "--", "ip", "netns", "add", server)
    created.append(server)
    ns(router, "sysctl", "-q", "-w", "net.ipv4.tcp_child_ehash_entries=0")
    for name, mask in zip(names, masks):
        if name in clients:
            priv("ip", "netns", "add", name)
            created.append(name)
        ns(name, "sysctl", "-q", "-w", f"net.core.rps_default_mask={cpu_mask(mask)}")
        ip(name, "link", "set", "lo", "up")
    ns(router, "sysctl", "-q", "-w", "net.ipv4.ip_forward=1")
    for i, (client, router_cpu) in enumerate(zip(clients, args.router_cpus), start=1):
        # A separate egress qdisc and its own RX CPU for each generator prevent
        # Linux's lone virtual-router NET_RX queue being the capacity limit.
        ns(router, "sysctl", "-q", "-w", f"net.core.rps_default_mask={cpu_mask(1 << router_cpu)}")
        if server_rx_masks is not None:
            mask = cpu_mask(expected_rx[i - 1]) or "0"
            ns(server, "sysctl", "-q", "-w", f"net.core.rps_default_mask={mask}")
        for endpoint, device, peer, octet in (
            (server, f"eth{i}", f"s{i}", 232),
            (client, "eth0", f"c{i}", 233),
        ):
            ip(endpoint, "link", "add", device, "type", "veth", "peer", "name", peer, "netns", router)
            ip(endpoint, "address", "add", f"10.{octet}.{i}.2/24", "dev", device)
            ip(router, "address", "add", f"10.{octet}.{i}.1/24", "dev", peer)
            ip(endpoint, "link", "set", device, "mtu", "1500", "up")
            ip(router, "link", "set", peer, "mtu", "1500", "up")
            ns(endpoint, "ethtool", "-K", device, "gro", "on", "gso", "on")
            ns(router, "ethtool", "-K", peer, "gro", "on", "gso", "on")
            ns(
                router,
                "tc",
                "qdisc",
                "add",
                "dev",
                peer,
                "root",
                "netem",
                "delay",
                f"{args.delay_us}us",
                "limit",
                "100000",
            )
        ip(client, "route", "add", "default", "via", f"10.233.{i}.1")
        ip(server, "route", "add", f"10.233.{i}.0/24", "via", f"10.232.{i}.1")
        for host in range(10, 10 + args.aliases):
            ip(server, "address", "add", f"10.232.{i}.{host}/32", "dev", f"eth{i}")
    probes = {}
    for name in names:
        interfaces = (
            ["eth1", "eth2", "eth3", "eth4"]
            if name == server
            else [f"{side}{i}" for side in ("s", "c") for i in range(1, 5)] if name == router else ["eth0"]
        )
        features = {}
        for iface in interfaces:
            info = ns(name, "ethtool", "-k", iface)
            features[iface] = {
                line.split(": ")[0]: line.split(": ")[1].split()[0]
                for line in info.splitlines()
                if line.startswith(
                    (
                        "generic-receive-offload:",
                        "generic-segmentation-offload:",
                        "tcp-segmentation-offload:",
                        "rx-checksumming:",
                        "tx-checksumming:",
                    )
                )
            }
            if any(
                features[iface].get(feature) != "on"
                for feature in ("generic-receive-offload", "generic-segmentation-offload")
            ):
                raise RuntimeError(f"GRO/GSO not enabled: {name} {iface} {features[iface]}")
        probes[name] = {
            "rx_masks": {
                iface: ns(name, "cat", f"/sys/class/net/{iface}/queues/rx-0/rps_cpus") for iface in interfaces
            },
            "offloads": features,
            "ehash_entries": ns(name, "cat", "/proc/sys/net/ipv4/tcp_ehash_entries"),
            "ephemeral": ns(name, "sysctl", "-n", "net.ipv4.ip_local_port_range"),
            "conntrack_entries": ns(name, "cat", "/proc/sys/net/netfilter/nf_conntrack_count", check=False),
        }
    for i, client in enumerate(clients, start=1):
        low, high = map(int, probes[client]["ephemeral"].split())
        probes[client]["source_destination_tuple_capacity"] = args.aliases * (high - low + 1)
        if probes[client]["source_destination_tuple_capacity"] < math.ceil(args.users / len(clients)):
            raise ValueError(
                "source-port range and destination aliases cannot support this shard's user count"
            )
        probes[client]["ping"] = ns(
            client, "ping", "-n", "-q", "-i", "0.01", "-c", "15", "-W", "1", f"10.232.{i}.10"
        )
    if args.server_ehash and int(probes[server]["ehash_entries"]) != args.server_ehash:
        raise RuntimeError(f"server namespace did not acquire requested TCP hash: {probes[server]}")
    if any(
        int(probes[server]["rx_masks"][f"eth{i}"].replace(",", ""), 16) != mask
        for i, mask in enumerate(expected_rx, start=1)
    ):
        raise RuntimeError("a server RX queue did not receive its requested CPU mask")
    for i, cpu in enumerate(args.router_cpus, start=1):
        if any(
            int(probes[router]["rx_masks"][f"{side}{i}"].replace(",", ""), 16) != 1 << cpu
            for side in ("s", "c")
        ):
            raise RuntimeError("router queue did not receive its private RX CPU")
    return probes


def router_network_sample(router):
    queues = json.loads(ns(router, "tc", "-j", "-s", "qdisc", "show"))
    links = json.loads(ns(router, "ip", "-j", "-s", "link", "show"))
    return {
        "router_rx": {
            row["ifname"]: {
                k: row.get("stats64", row.get("stats", {})).get("rx", {}).get(k, 0)
                for k in ("packets", "dropped", "errors")
            }
            for row in links
            if row["ifname"] != "lo"
        },
        "router_tx": {
            row["ifname"]: {
                k: row.get("stats64", row.get("stats", {})).get("tx", {}).get(k, 0)
                for k in ("packets", "dropped", "errors")
            }
            for row in links
            if row["ifname"] != "lo"
        },
        "router_queues": {
            q["dev"]: {
                k: q.get(k, 0)
                for k in (
                    "drops",
                    "overlimits",
                    "requeues",
                    "backlog",
                    "qlen",
                    "packets",
                    "bytes",
                )
            }
            for q in queues
            if q["kind"] == "netem"
        },
    }


def host_is_quiet(sample, args):
    return sample["orphan"] < args.max_idle_orphans and sample["alloc"] < args.max_idle_alloc


def wait_for_prior_teardown(out, args):
    start = time.monotonic()
    consecutive = 0
    readings = []
    timeout = args.quiet_timeout
    while True:
        now = time.monotonic()
        sample = {"elapsed_s": round(now - start, 3), **host_tcp_counters()}
        readings.append(sample)
        consecutive = consecutive + 1 if host_is_quiet(sample, args) else 0
        report = {
            "passed": consecutive >= 2,
            "elapsed_s": sample["elapsed_s"],
            "threshold_orphans_exclusive": args.max_idle_orphans,
            "threshold_alloc_exclusive": args.max_idle_alloc,
            "consecutive_1s_readings_required": 2,
            "timeout_s": timeout,
            "start": readings[0],
            "end": sample,
        }
        write(out / "preflight.json", report)
        with (out / "preflight.jsonl").open("a") as log:
            print(json.dumps(sample), file=log)
        if report["passed"] or now - start >= timeout:
            return report
        time.sleep(1)


def residual_socket_states(namespace, residual):
    output = {"sampled_unix": time.time(), "tcp_curr_estab_before_ss": residual, "service_port": 80}
    if not 0 < residual <= 10000:
        output["skipped"] = "service residual outside the bounded diagnostic range"
        return output
    for state in ("established", "close-wait"):
        try:
            data = ns(namespace, "ss", "-Htn", "state", state, "sport", "=", ":80", timeout=4)
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            output[state] = {"error": str(error)}
            continue
        seen = []
        for line in data.splitlines():
            parts = line.split()
            if parts and not parts[0].isdigit():
                parts = parts[1:]
            if len(parts) >= 4 and parts[0].isdigit() and parts[1].isdigit():
                seen.append((int(parts[0]), int(parts[1]), parts[2], parts[3]))
        output[state] = {
            "count": len(seen),
            "recvq_bytes": sum(v[0] for v in seen),
            "sendq_bytes": sum(v[1] for v in seen),
            "nonempty_recvq": sum(v[0] > 0 for v in seen),
            "nonempty_sendq": sum(v[1] > 0 for v in seen),
            "examples": [
                {"recvq": rx, "sendq": tx, "local": local, "peer": peer}
                for rx, tx, local, peer in sorted(seen, key=lambda r: -(r[0] + r[1]))[:6]
            ],
        }
    return output


def post_score_drain(args, out, unit, names, baseline):
    start = time.monotonic()
    service_deadline = start + args.drain_timeout * 2 / 3
    deadline = start + args.drain_timeout
    consecutive = 0
    initial = None
    stopped = False
    stop_reason = None
    stable_residual = None
    stable_since = start
    endpoints = [names[0], *names[2:]]
    while True:
        ts = time.monotonic()
        state = {}
        for name in endpoints:
            data = ns(name, "cat", "/proc/net/snmp", check=False).splitlines()
            for head, vals in zip(data[::2], data[1::2]):
                if head.startswith("Tcp:"):
                    state[name] = int(vals.split()[head.split().index("CurrEstab")])
        service = run("systemctl", "is-active", unit, check=False)
        sample = {
            "elapsed_s": round(ts - start, 3),
            "service_state": service,
            "service_stopped": stopped,
            "namespaced_established": state,
            "host_tcp": host_tcp_counters(),
        }
        if initial is None:
            initial = sample
        complete = len(state) == len(endpoints)
        closed = complete and not any(state.values())
        clients_closed = complete and not any(state[n] for n in endpoints[1:])
        residual = (
            state.get(endpoints[0]) if clients_closed and host_is_quiet(sample["host_tcp"], args) else None
        )
        if residual is None or residual != stable_residual:
            stable_since = ts
        stable_residual = residual
        stalled = residual is not None and residual < args.max_idle_alloc and ts - stable_since >= 6
        if not stopped:
            if closed:
                stop_reason = "all endpoint sockets closed"
            elif service not in ("active", "activating"):
                stop_reason = "service exited"
            elif stalled:
                stop_reason = (
                    "all generators closed and a small residual service count stabilized for six seconds"
                )
            elif ts >= service_deadline:
                stop_reason = "service drain time budget reached"
        sample["stop_reason"] = stop_reason
        with (out / "drain.jsonl").open("a") as fp:
            print(json.dumps(sample), file=fp)
        if not stopped and stop_reason:
            if stalled and residual:
                write(out / "drain-residual-sockets.json", residual_socket_states(endpoints[0], residual))
            priv("systemctl", "stop", "--no-block", unit, check=False)
            stopped = True
            consecutive = 0
        elif stopped:
            inactive = service not in ("active", "activating", "deactivating", "reloading")
            consecutive = (
                consecutive + 1 if closed and inactive and host_is_quiet(sample["host_tcp"], args) else 0
            )
        finished = consecutive >= 2
        if finished or ts >= deadline:
            write(
                out / "drain.json",
                {
                    "idle_before_build": baseline["end"],
                    "initial": initial,
                    "final": sample,
                    "drained": finished,
                    "service_stop_reason": stop_reason,
                    "timeout_s": args.drain_timeout,
                    "elapsed_s": sample["elapsed_s"],
                    "stock_shards_completed": sum(
                        (out / f"shard-{i}.json").is_file() for i in range(len(args.groups))
                    ),
                },
            )
            return
        time.sleep(1 if closed else 2)


def main(network_setup=set_up, network_profile="four-router-lanes", network_sample=None, delay_us=48,
         service_properties=(), inspect_service=None):
    p = argparse.ArgumentParser()
    p.add_argument("--server", required=True, type=Path)
    p.add_argument("--cwd", type=Path)
    p.add_argument("--env", action="append", default=[])
    p.add_argument("--users", required=True, type=int)
    p.add_argument("--hold", required=True, type=int)
    p.add_argument("--quiet-timeout", type=float, default=600)
    p.add_argument("--max-idle-orphans", type=int, default=1000)
    p.add_argument("--max-idle-alloc", type=int, default=10000)
    p.add_argument("--drain-timeout", type=float, default=180)
    p.add_argument("--ramp", type=int)
    p.add_argument("--delay-us", type=int, default=delay_us)
    p.add_argument("--server-ehash", type=int, default=16384, help="Use 0 for the host-shared control")
    p.add_argument("--aliases", type=int, default=8)
    p.add_argument("--app-cpu", type=int, default=APP_CPU)
    p.add_argument(
        "--router-cpus",
        default=",".join(map(str, ROUTER_CPUS)),
        help="One dedicated router CPU per lane, comma separated",
    )
    p.add_argument(
        "--client-cpus",
        default=";".join(GROUPS),
        help="Four semicolon-separated groups of comma-separated CPU numbers",
    )
    p.add_argument("--manager-cpu", type=int, default=MANAGER_CPU)
    p.add_argument("--tag", required=True)
    p.add_argument("--output", type=Path, default=HERE.parent / "runs")
    p.add_argument("--mload", type=Path, default=MLOAD)
    p.add_argument("--seed", type=Path, default=ROOT / "seed/feed.db")
    p.add_argument("--tokens", type=Path, default=ROOT / "seed/tokens.json")
    a = p.parse_args()
    a.router_cpus = [int(s) for s in a.router_cpus.split(",")]
    a.groups = a.client_cpus.split(";")
    clients_cpu_list = [[int(s) for s in group.split(",")] for group in a.groups]
    cpus = [a.app_cpu, a.manager_cpu, *a.router_cpus, *[cpu for group in clients_cpu_list for cpu in group]]
    if (
        len(a.router_cpus) != 4
        or len(a.groups) != 4
        or len(set(cpus)) != len(cpus)
        or not set(cpus) <= os.sched_getaffinity(0)
    ):
        p.error(
            "provide four disjoint router and client groups from this process' affinity, plus dedicated app and manager CPUs"
        )
    a.server = a.server.resolve(strict=True)
    a.seed = a.seed.resolve(strict=True)
    a.tokens = a.tokens.resolve(strict=True)
    a.mload = a.mload.resolve(strict=True)
    if not a.mload.is_file() or not os.access(a.mload, os.X_OK):
        p.error("pass the compiled organizer load generator using --mload")
    a.cwd = a.cwd.resolve() if a.cwd else a.server.parent
    a.ramp = a.ramp if a.ramp is not None else max(60, math.ceil(3 * a.users / 10000))
    if a.users < len(a.groups) or a.hold < 6 or a.ramp <= 0 or not 1 <= a.aliases <= 128:
        p.error("invalid trial dimensions")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", a.tag):
        p.error("tag must be a filename component")
    if os.getuid() == 0:
        p.error("invoke as the ordinary benchmark user")
    if sha(a.mload) != MLOAD_SHA:
        raise RuntimeError("unexpected organizer scorer SHA-256")
    mutex_path = Path(tempfile.gettempdir()) / f"twelve-router-{os.getuid()}.lock"
    mutex = os.open(mutex_path, os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    fcntl.flock(mutex, fcntl.LOCK_EX)
    os.sched_setaffinity(0, {a.manager_cpu})
    out = a.output.resolve() / a.tag
    out.mkdir(parents=True, exist_ok=False)
    if min(a.quiet_timeout, a.max_idle_orphans, a.max_idle_alloc, a.drain_timeout) <= 0:
        p.error("quiet and drain limits must all be positive")
    idle = wait_for_prior_teardown(out, a)
    if not idle["passed"]:
        reason = "prior host-wide TCP sockets did not reach the configured idle thresholds before this trial"
        write(
            out / "shape.json",
            {
                "tag": a.tag,
                "users": a.users,
                "ramp_s": a.ramp,
                "hold_s": a.hold,
                "preflight": idle,
                "topology": network_profile,
            },
        )
        write(
            out / "summary.json",
            {
                "tag": a.tag,
                "users": a.users,
                "eligible": False,
                "qualified": False,
                "stock_pass": None,
                "infrastructure_flags": [reason],
                "preflight": idle,
            },
        )
        raise RuntimeError(reason)
    ns_prefix = "tr" + uuid.uuid4().hex[:7]
    names = [ns_prefix + suffix for suffix in ("s", "r", "c0", "c1", "c2", "c3")]
    server, router, *clients = names
    unit = f"twelve-revisit-{ns_prefix}.service"
    user = pwd.getpwuid(os.getuid()).pw_name
    created, jobs, monitors, logs, samples = [], [], [], [], []

    def stop_signal(n, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop_signal)
    try:
        topology = network_setup(a, names, created)
        napi_pids = [
            entry["pid"]
            for interface in topology[server].get("threaded_napi", {}).values()
            for entry in interface["napi"]
        ]
        write(
            out / "shape.json",
            {
                "utc_started": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "namespaces": names,
                "network": topology,
                "router_delay_us_each_direction": a.delay_us,
                "app_cpu": a.app_cpu,
                "server_ehash_requested": a.server_ehash,
                "router_cpus": a.router_cpus,
                "client_cpus": a.groups,
                "manager_cpu": a.manager_cpu,
                "topology": network_profile,
                "preflight": idle,
                "users": a.users,
                "ramp_s": a.ramp,
                "hold_s": a.hold,
                "user_offsets": "disjoint",
                "server": str(a.server),
                "cwd": str(a.cwd),
                "server_sha256": sha(a.server),
                "mload_sha256": MLOAD_SHA,
                "seed_sha256": sha(a.seed),
                "tokens_sha256": sha(a.tokens),
                "target_aliases": a.aliases,
                "unit": unit,
                "service_user": user,
                "app_memory_bytes": 1750 * 1024**2,
                "fd_limit": 65535,
                "host": os.uname().nodename,
                "kernel": os.uname().release,
                "server_environment": a.env,
                "cpu_description": json.loads(run("lscpu", "-J")),
                "host_initial_meminfo": Path("/proc/meminfo").read_text(),
            },
        )
        props = [
            f"User={user}",
            "NoNewPrivileges=yes",
            "LimitNOFILE=65535:65535",
            "MemoryMax=1750M",
            "MemorySwapMax=0",
            "CPUAccounting=yes",
            f"CPUAffinity={a.app_cpu}",
            f"AllowedCPUs={a.app_cpu}",
            f"NetworkNamespacePath=/run/netns/{server}",
            "CapabilityBoundingSet=CAP_NET_BIND_SERVICE",
            "AmbientCapabilities=CAP_NET_BIND_SERVICE",
            f"WorkingDirectory={a.cwd}",
            f"StandardOutput=append:{out}/server.log",
            f"StandardError=append:{out}/server.log",
            *service_properties,
        ]
        env = ["HOST=0.0.0.0", "PORT=80", "JWT_SECRET=twelve-dollar-challenge", *a.env]
        priv(
            "systemd-run",
            "--quiet",
            "--collect",
            f"--unit={unit}",
            *[f"--property={prop}" for prop in props],
            *[f"--setenv={s}" for s in env],
            "/usr/bin/python3",
            str(HERE / "launch.py"),
            str(out),
            str(a.seed),
            str(a.server),
        )
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            health = ns(
                clients[0],
                "curl",
                "--noproxy",
                "*",
                "-fsS",
                "--max-time",
                "1",
                "http://10.232.1.10/health",
                check=False,
            )
            if '"status":"ok"' in health.replace(" ", ""):
                break
            if run("systemctl", "is-active", unit, check=False) not in ("active", "activating"):
                raise RuntimeError(f"server exited on startup: {(out / 'server.log').read_text()[-1800:]}")
            time.sleep(0.15)
        else:
            raise RuntimeError("server did not become healthy within 60 seconds")
        group = run("systemctl", "show", unit, "--property=ControlGroup", "--value")
        cg = Path("/sys/fs/cgroup") / group.lstrip("/")
        if (cg / "memory.max").read_text().strip() != str(1750 * 1024**2):
            raise RuntimeError("server cgroup did not receive the required memory limit")
        if inspect_service is not None:
            write(out / "service-controls.json", inspect_service(cg, a))
        opened = json.loads((out / "app-start.json").read_text())
        if opened["seed_sha256"] != sha(a.seed):
            raise RuntimeError("database copy differs from the pristine source seed")
        server_pid = opened["pid"]
        start = int((time.time() + 4) * 1000)
        write(
            out / "timing.json",
            {
                "stock_start_unix": start / 1000,
                "hold_starts_unix": start / 1000 + a.ramp,
                "cutoff_unix": start / 1000 + a.ramp + a.hold,
            },
        )
        for i, (client, cpus) in enumerate(zip(clients, a.groups)):
            targets = ",".join(f"10.232.{i+1}.{host}" for host in range(10, 10 + a.aliases))
            count = a.users // len(a.groups) + int(i < a.users % len(a.groups))
            offset = (a.users // len(a.groups)) * i + min(i, a.users % len(a.groups))
            log = (out / f"client-{i}.log").open("w")
            logs.append(log)
            cmd = [
                "sudo",
                "-n",
                "ip",
                "netns",
                "exec",
                client,
                "prlimit",
                "--nofile=262144:262144",
                "setpriv",
                f"--reuid={os.getuid()}",
                f"--regid={os.getgid()}",
                "--init-groups",
                "--inh-caps=-all",
                "--ambient-caps=-all",
                "--bounding-set=-all",
                "--no-new-privs",
                "taskset",
                "-c",
                cpus,
                "/usr/bin/env",
                f"GOMAXPROCS={len(cpus.split(','))}",
                str(a.mload),
                "-target",
                targets,
                "-ports",
                "80",
                "-users",
                str(count),
                "-offset",
                str(offset),
                "-ramp",
                f"{a.ramp}s",
                "-hold",
                f"{a.hold}s",
                "-start",
                str(start),
                "-timeout",
                "60s",
                "-think-scale",
                "1",
                "-tokens",
                str(a.tokens),
                "-out",
                str(out / f"shard-{i}.json"),
            ]
            jobs.append(subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT))
            ping_log = (out / f"rtt-{i}.log").open("w")
            logs.append(ping_log)
            monitors.append(
                subprocess.Popen(
                    [
                        "sudo",
                        "-n",
                        "ip",
                        "netns",
                        "exec",
                        client,
                        "taskset",
                        "-c",
                        cpus,
                        "ping",
                        "-n",
                        "-D",
                        "-i",
                        "1",
                        "-W",
                        "1",
                        "-c",
                        str(a.ramp + a.hold + 6),
                        f"10.232.{i+1}.10",
                    ],
                    stdout=ping_log,
                    stderr=subprocess.STDOUT,
                )
            )
        until = start / 1000 + a.ramp + a.hold
        previous_net_sample = 0
        while True:
            sample = {
                "unix": time.time(),
                "app": memstats(cg),
                "cpus": tcp_counts(a.app_cpu, a.router_cpus, a.manager_cpu),
            }
            samples.append(sample)
            with (out / "samples.jsonl").open("a") as fp:
                print(json.dumps(sample), file=fp)
            with (out / "telemetry.jsonl").open("a") as fp:
                print(json.dumps(snapshot(server_pid, cg, napi_pids)), file=fp)
            now = time.time()
            if now - previous_net_sample >= 8 or (
                until - 2.5 < now < until and previous_net_sample < until - 2.5
            ):
                previous_net_sample = now
                external = network_sample(router) if network_sample else router_network_sample(router)
                net_state = {
                    "unix": time.time(),
                    "collection_started_unix": now,
                    **external,
                    "socket_queue_sample": (
                        sample_socket_queues(server_pid) if now >= start / 1000 + 15 else {}
                    ),
                }
                with (out / "network-samples.jsonl").open("a") as fp:
                    print(json.dumps(net_state), file=fp)
            if all(j.poll() is not None for j in jobs):
                break
            if time.time() > until + 55:
                raise RuntimeError("a stock client did not terminate on schedule")
            time.sleep(min(2, max(0.15, until + 0.2 - time.time())))
        if any(j.returncode for j in jobs):
            raise RuntimeError("a stock client failed; see client logs")
        shards = [out / f"shard-{i}.json" for i in range(len(a.groups))]
        run(a.mload, "-merge", "-out", out / "merged.json", *shards)
        results = [json.loads(f.read_text()) for f in shards]
        merged = json.loads((out / "merged.json").read_text())
        final_counts = []
        for row in results:
            trace = row["timeline"]
            final_counts.append(
                {
                    "users": max(t["users"] for t in trace),
                    "conns": max(t["conns"] for t in trace),
                    "last_conns": trace[-1]["conns"],
                    "dials": row["dials"],
                }
            )
        max_users = sum(s["users"] for s in final_counts)
        established = sum(s["conns"] for s in final_counts)
        pre_cut = [s["app"] for s in samples if s["unix"] <= until]
        oom = max((s.get("memory.events", {}).get("oom_kill", 0) for s in pre_cut), default=0)
        app_alive = run("systemctl", "is-active", unit, check=False) == "active"
        compact = {
            k: v
            for k, v in merged.items()
            if k
            in (
                "users",
                "duration_s",
                "reqs",
                "fails",
                "req_per_s",
                "err_pct",
                "p95_ms",
                "p99_ms",
                "max_ms",
                "pass",
                "errors",
                "dials",
                "connect_p99_ms",
            )
        }
        compact.update(
            tag=a.tag,
            ramp_s=a.ramp,
            hold_s=a.hold,
            users_active=max_users,
            peak_shard_established_sum=established,
            score_sources=final_counts,
            server_alive_at_client_exit=app_alive,
            oom_kill_at_cutoff=oom,
            eligible=bool(
                merged["users"] == a.users == max_users
                and not oom
                and merged["duration_s"] >= a.hold + a.ramp - 0.3
            ),
            app_peak_mib=round(max(s.get("memory.peak", 0) for s in pre_cut) / 1024**2, 1),
        )
        compact["qualified"] = compact["pass"] and compact["eligible"]
        write(out / "summary.json", compact)
        for name in names:
            (out / f"net-{name}.txt").write_text(ns(name, "nstat", "-asz", check=False))
            write(out / f"interfaces-{name}.json", json.loads(ns(name, "ip", "-j", "-s", "link", "show")))
        if network_sample:
            write(out / "external-at-client-exit.json", network_sample(router))
        else:
            report = {
                dev: ns(router, "tc", "-s", "qdisc", "show", "dev", dev)
                for dev in [f"{side}{i}" for side in ("s", "c") for i in range(1, 5)]
            }
            write(out / "router-queues.json", report)
        assessed = assess(out)
        print(
            json.dumps(
                {
                    k: assessed[k]
                    for k in (
                        "tag",
                        "users",
                        "hold_s",
                        "qualified",
                        "stock_pass",
                        "scored_healthy_environment",
                        "wire_eligible",
                        "users_active",
                        "p95_ms",
                        "p99_ms",
                        "req_per_s",
                        "err_pct",
                        "infrastructure_flags",
                    )
                }
            ),
            flush=True,
        )
    finally:
        for job in jobs + monitors:
            if job.poll() is None:
                job.terminate()
        for job in jobs + monitors:
            try:
                job.wait(timeout=2)
            except subprocess.TimeoutExpired:
                job.kill()
        for fp in logs:
            fp.close()
        try:
            if jobs:
                post_score_drain(a, out, unit, names, idle)
        finally:
            priv("systemctl", "stop", unit, check=False)
            for name in reversed(created):
                priv("ip", "netns", "del", name, check=False)
            for db in ("feed.db", "feed.db-wal", "feed.db-shm", "feed.db-journal"):
                (out / db).unlink(missing_ok=True)


if __name__ == "__main__":
    main()
