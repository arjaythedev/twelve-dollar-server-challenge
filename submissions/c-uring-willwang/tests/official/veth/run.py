"""Measure one native server and genuine stock Go with kernel-only private veth receiver steering."""

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from common import (
    core_counts,
    docker,
    group_counts,
    group_of,
    run,
    stop,
    subnet_request,
    tcp_sockstat,
    wait_until,
    validate_cpus,
)
from probe import PrivatePairs
from closure import CloseObservation, process_alive, check_health, check_database, close_checks_pass


def as_kv(text):
    return {k: int(v) for k, v in (part.split("=") for part in text.split())}


def parallel(fn, args):
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(12, len(args)))) as p:
        return list(p.map(fn, args))


def sha(path):
    with open(path, "rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def global_tcp():
    line = next(x for x in Path("/proc/net/sockstat").read_text().splitlines() if x.startswith("TCP:"))
    t = line.split()[1:]
    return {k: int(v) for k, v in zip(t[::2], t[1::2])}


def host_ct():
    return int(Path("/proc/sys/net/netfilter/nf_conntrack_count").read_text())


def ns_metrics(provider, code=b"T"):
    return as_kv(subnet_request(provider, code))


def emit(path, obj):
    path.write_text(json.dumps(obj, indent=2) + "\n")


def launch(args, pairs, root):
    providers = pairs.providers
    ng = len(pairs.groups)
    nl = len(providers)
    budget = args.memory_mib
    cpu = args.server_cpu
    unique = pairs.unique
    unit = unique + ".scope"
    env = dict(os.environ)
    env.setdefault("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")
    scope = None
    service = None
    api = None
    pidfd = None
    armed = False
    go_started = False
    observation = None
    seed_sha = sha(args.seed)
    tokens_sha = sha(args.tokens)
    before = {
        "global": global_tcp(),
        "conntrack": host_ct(),
        "inuse": [tcp_sockstat(p)["inuse"] for p in providers],
    }
    try:
        cfg = {
            "seed": str(args.seed),
            "db": str(args.db),
            "pid": str(root / "api.pid"),
            "cpu": cpu,
            "secret": args.secret,
            "root": str(root),
            "preload": str(args.work / "veth-bin/allocate.so"),
            "listeners": nl,
            "server": str(args.server.resolve()),
        }
        plan = root / "launch.json"
        emit(plan, cfg)
        cmd = [
            "systemd-run",
            "--user",
            "--scope",
            "--quiet",
            "--unit",
            unit,
            "-p",
            f"MemoryMax={budget}M",
            "-p",
            "MemorySwapMax=0",
            "taskset",
            "-c",
            str(cpu),
            sys.executable,
            str(HERE / "launch.py"),
            str(plan),
        ]
        with (args.out / "server.log").open("w") as log:
            service = subprocess.Popen(cmd, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT)
        wait_until(
            lambda: Path(cfg["pid"]).exists() and all(subnet_request(p, b"L") == "1" for p in providers),
            15,
            lambda: "server failed to open every true listener: " + (args.out / "server.log").read_text(),
        )
        api = int(Path(cfg["pid"]).read_text())
        pidfd = os.pidfd_open(api)
        status = dict(
            line.split(":", 1)
            for line in Path(f"/proc/{api}/status").read_text().splitlines()
            if ":" in line
        )
        if int(status["CapEff"], 16) or int(status["CapPrm"], 16) or 0 in map(int, status["Uid"].split()):
            raise ValueError("the native server requires an ordinary user with zero capabilities")
        scope = group_of(api)
        if (
            int((scope / "memory.max").read_text()) != budget * 1024**2
            or int((scope / "memory.swap.max").read_text())
            or os.sched_getaffinity(api) != {cpu}
        ):
            raise ValueError("server CPU/memory/swap drift")
        line = next(
            l
            for l in Path(f"/proc/{api}/limits").read_text().splitlines()
            if l.startswith("Max open files")
        )
        if line.split()[3:5] != ["65535", "65535"]:
            raise ValueError("native fd limit differs: " + line)
        copy_hash = (root / "seed.verified").read_text().strip()
        if copy_hash != seed_sha:
            raise ValueError("the pre-exec, in-scope seed copy must match")
        pids = parallel(
            lambda c: int(docker("inspect", "--format", "{{.State.Pid}}", c)),
            pairs.server_ids + pairs.client_ids,
        )
        helpers = [group_of(p) for p in pids[:nl]]
        clients = [group_of(p) for p in pids[nl:]]
        if any(os.sched_getaffinity(p) != set(group) for p, group in zip(pids[nl:], pairs.groups)):
            raise ValueError("generator CPU drift")
        start = time.time_ns() // 1000000 + max(3000, 300 * ng)
        offset = 0
        jobs = []
        for k, (client, group) in enumerate(zip(pairs.client_ids, pairs.groups)):
            users = args.users // ng + (k < args.users % ng)
            targets = [
                f"10.241.{k*args.targets_per_client+i+1}.{host}"
                for i in range(args.targets_per_client)
                for host in range(1, args.aliases + 1)
            ]
            jobs.append(
                (
                    client,
                    [
                        "exec",
                        "-d",
                        "--env",
                        f"GOMAXPROCS={len(group)}",
                        client,
                        "/hold",
                        "/mload",
                        "-target",
                        ",".join(targets),
                        "-ports",
                        "19432",
                        "-tokens",
                        "/tokens.json",
                        "-users",
                        str(users),
                        "-offset",
                        str(offset),
                        "-start",
                        str(start),
                        "-ramp",
                        f"{args.ramp}s",
                        "-hold",
                        f"{args.hold}s",
                        "-out",
                        "/out/score.json",
                    ],
                )
            )
            offset += users
        parallel(lambda j: docker(*j[1]), jobs)
        go_started = True
        if time.time_ns() // 1000000 >= start:
            raise ValueError("failed to prepare synchronized Go clients before their start deadline")
        shape = {
            "users": args.users,
            "ramp": args.ramp,
            "hold": args.hold,
            "start_ms": start,
            "server_cpu": cpu,
            "manager_cpu": args.manager_cpu,
            "native_uid": int(status["Uid"].split()[0]),
            "native_effective_caps": int(status["CapEff"], 16),
            "native_permitted_caps": int(status["CapPrm"], 16),
            "groups": pairs.groups,
            "listeners": nl,
            "server_memory_mib": budget,
            "network_helper_memory_bound_mib": nl * 8,
            "source_tuples": args.tuples,
            "native_pid": api,
            "server_scope": unit,
            "server_sha256": sha(args.server),
            "scorer_sha256": sha(args.work / "mload"),
            "seed_sha256": seed_sha,
            "native_preexec_seed_sha256": copy_hash,
            "post_open_full_database_hash": False,
            "observer_note": "Pre-exec in-scope seed copy SHA is authoritative. Scanning the live database afterward would populate cold pages in the measurement parent cgroup.",
            "seed_bytes": args.seed.stat().st_size,
            "tokens_sha256": tokens_sha,
            "weighted_stock_aggregate": True,
            "native_hard_soft_file_limit": line,
            "before": before,
            "target_ips_per_source": args.targets_per_client * args.aliases,
            "physical_hz": os.sysconf("SC_CLK_TCK"),
        }
        emit(args.out / "shape.json", shape)
        if args.verify_close:
            observation = CloseObservation(
                scope,
                args.db,
                pidfd,
                start + 1000 * (args.ramp + args.hold),
                args.out,
                (pairs.providers, pairs.client_providers),
            )
        print(f"stock scheduled {start} ({args.users} true VUs)", flush=True)
        used_cpus = set([cpu] + [c for group in pairs.groups for c in group])
        with (args.out / "samples.jsonl").open("w") as out:
            while not all((p / "exit").exists() for p in pairs.reports):
                if service.poll() is not None and time.time_ns() // 1000000 < start + 1000 * (
                    args.ramp + args.hold
                ):
                    raise RuntimeError("native server exited before the scheduled scorer cutoff")
                if time.time_ns() // 1000000 > start + 1000 * (args.ramp + args.hold + 60):
                    raise RuntimeError("stock generator completion deadline")
                sample = {
                    "time_ns": time.time_ns(),
                    "mono_ns": time.monotonic_ns(),
                    "native": group_counts(scope),
                    "clients": [group_counts(g) for g in clients],
                    "helpers": [group_counts(g) for g in helpers],
                    "cores": core_counts(used_cpus),
                    "tcp": [ns_metrics(p) for p in providers],
                    "interfaces": [ns_metrics(p, b"I") for p in providers],
                    "client_tcp": [ns_metrics(p) for p in pairs.client_providers],
                    "client_interfaces": [
                        [ns_metrics(p, bytes([48 + k])) for k in range(args.targets_per_client)]
                        for p in pairs.client_providers
                    ],
                    "netns_conntrack": [int(subnet_request(p, b"?")) for p in providers],
                    "host_conntrack": host_ct(),
                    "global": global_tcp(),
                }
                print(json.dumps(sample), file=out, flush=True)
                time.sleep(args.sample)
        shard_exit = [int((p / "exit").read_text()) for p in pairs.reports]
        if any(shard_exit) and not args.verify_close:
            raise RuntimeError(f"stock Go shard exits: {shard_exit}")
        missing_reports = [k for k, d in enumerate(pairs.reports) if not (d / "score.json").is_file()]
        if missing_reports and not args.verify_close:
            raise RuntimeError(f"missing stock Go reports from shards {missing_reports}")
        if missing_reports:
            # Preserve every actual shard output and lifecycle evidence without
            # publishing a merged score from an incomplete original workload.
            print(f"incomplete stock score; missing shards {missing_reports}", file=sys.stderr, flush=True)
        else:
            run(
                str(args.work / "mload"),
                "-merge",
                "-out",
                str(args.out / "score.json"),
                *(str(d / "score.json") for d in pairs.reports),
            )
            report = json.loads((args.out / "score.json").read_text())
            print(
                json.dumps(
                    {
                        k: report[k]
                        for k in (
                            "users",
                            "dials",
                            "reqs",
                            "req_per_s",
                            "p95_ms",
                            "p99_ms",
                            "fails",
                            "pass",
                        )
                    }
                ),
                flush=True,
            )
        raw = {
            "native": group_counts(scope),
            "clients": [group_counts(g) for g in clients],
            "helpers": [group_counts(g) for g in helpers],
            "native_tcp": [ns_metrics(p) for p in providers],
            "interfaces": [ns_metrics(p, b"I") for p in providers],
            "host_conntrack": host_ct(),
            "global": global_tcp(),
        }
        emit(args.out / "end.json", raw)
        armed = True
    finally:
        try:
            if api and go_started and armed:
                # Let normal native io_uring see the Go peer EOFs before releasing
                # listeners, then preserve ALL private devices through kernel FIN drain.
                end = time.monotonic() + 20
                if args.verify_close:
                    with (args.out / "close-tcp.jsonl").open("w") as closing:
                        while True:
                            snmp = [ns_metrics(p).get("CurrEstab", 0) for p in providers]
                            client_snmp = [
                                ns_metrics(p).get("CurrEstab", 0) for p in pairs.client_providers
                            ]
                            print(
                                json.dumps(
                                    {
                                        "utc": time.time(),
                                        "native_established_plus_close_wait": snmp,
                                        "client_established_plus_close_wait": client_snmp,
                                        "native_alive": process_alive(pidfd),
                                    }
                                ),
                                file=closing,
                                flush=True,
                            )
                            if not sum(snmp) or time.monotonic() >= end:
                                break
                            time.sleep(0.25)
                else:
                    while (
                        time.monotonic() < end
                        and sum(ns_metrics(p).get("CurrEstab", 0) for p in providers) > 0
                    ):
                        time.sleep(0.25)
            if args.verify_close and api and go_started and armed:
                post = {
                    "pid": api,
                    "native_alive_before_health": process_alive(pidfd),
                    "utc": time.time(),
                    "official_shard_exit": shard_exit,
                    "missing_stock_reports": missing_reports,
                    "native_by_link_established_plus_close_wait": [
                        ns_metrics(p).get("CurrEstab", 0) for p in providers
                    ],
                    "client_by_shard_established_plus_close_wait": [
                        ns_metrics(p).get("CurrEstab", 0) for p in pairs.client_providers
                    ],
                }
                post["health"] = (
                    [
                        check_health(
                            pairs.client_providers[k // args.targets_per_client], f"10.241.{k+1}.1"
                        )
                        for k in range(nl)
                    ]
                    if process_alive(pidfd)
                    else []
                )
                post["native_alive_after_health"] = process_alive(pidfd)
                # This independent read-only check is after the scored client process exit.
                # It can page the physical database into memory, so record its phase explicitly.
                post["integrity_start_utc"] = time.time()
                emit(args.out / "post-close.json", post)
                post["live_db_verification"] = check_database(args.db, timeout=0.15)
                post["native_alive_after_integrity"] = process_alive(pidfd)
                post["intentional_stop_utc"] = time.time()
                emit(args.out / "post-close.json", post)
            if api:
                try:
                    if pidfd is not None:
                        signal.pidfd_send_signal(pidfd, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            if scope and go_started and armed:
                with (args.out / "drain.jsonl").open("w") as log:
                    drain_started = time.monotonic()
                    drained = False
                    next_msg = 0
                    while True:
                        values = [tcp_sockstat(p) for p in providers]
                        now = time.monotonic()
                        glob = global_tcp()
                        print(
                            json.dumps(
                                {"mono": now, "native_inuse": [v["inuse"] for v in values], "global": glob}
                            ),
                            file=log,
                            flush=True,
                        )
                        complete = all(s["inuse"] <= count for s, count in zip(values, before["inuse"]))
                        if (
                            complete
                            and glob["orphan"] <= before["global"]["orphan"] + 32
                            and glob["alloc"] <= before["global"]["alloc"] + 128
                        ):
                            drained = True
                            break
                        if now - drain_started >= args.drain_timeout:
                            break
                        if now > next_msg:
                            print(
                                "draining real private TCP",
                                sum(v["inuse"] for v in values),
                                glob["orphan"],
                                glob["alloc"],
                                flush=True,
                            )
                            next_msg = now + 15
                        time.sleep(0.5)
                time.sleep(0.2)
                emit(
                    args.out / "drain.json",
                    {
                        "completed": drained,
                        "duration_s": time.monotonic() - drain_started,
                        "native_inuse": [tcp_sockstat(p)["inuse"] for p in providers],
                        "host_conntrack": host_ct(),
                        "global": global_tcp(),
                    },
                )
            if args.verify_close and api and go_started and armed:
                post["poststop_db_verification"] = check_database(args.db)
                post["native_alive_after_stop"] = process_alive(pidfd)
                post["checks_pass"] = not missing_reports and close_checks_pass(post, nl, ng)
                emit(args.out / "post-close.json", post)
                print("post-close verification", "passed" if post["checks_pass"] else "failed", flush=True)
            for p in pairs.reports:
                (p / "release").touch()
        finally:
            if observation is not None:
                observation.stop()
            if pidfd is not None:
                os.close(pidfd)
            if service:
                try:
                    subprocess.run(
                        ["systemctl", "--user", "stop", unit], env=env, capture_output=True, timeout=10
                    )
                finally:
                    stop(service)
                # A user scope can vanish after simultaneous client close, taking its
                # memory counters with it. Preserve any journal OOM event and UTC time.
                try:
                    journal = run(
                        "journalctl",
                        "--user",
                        "--user-unit",
                        unit,
                        "--no-pager",
                        "--utc",
                        "-o",
                        "short-iso-precise",
                        check=False,
                        timeout=5,
                    )
                except (FileNotFoundError, subprocess.TimeoutExpired):
                    journal = ""
                if journal:
                    (args.out / "service-journal.log").write_text(journal + "\n")

    return post["checks_pass"] if args.verify_close else None


def main(args):
    validate_cpus(args.manager_cpu, args.server_cpu, args.groups)
    args.seed = args.seed.resolve()
    args.tokens = args.tokens.resolve()
    if (
        args.out.exists()
        or not args.server.is_file()
        or not args.seed.is_file()
        or not args.tokens.is_file()
        or args.ramp <= 0
        or args.hold <= 0
        or args.sample <= 0
        or args.drain_timeout <= 0
    ):
        raise ValueError("use existing inputs, fresh output and positive durations")
    n = len(args.groups) * args.targets_per_client
    if (
        not 1 <= n <= 8
        or not 1 <= args.aliases <= 64
        or args.users < len(args.groups)
        or args.server_cpu in [c for g in args.groups for c in g]
    ):
        raise ValueError("invalid independent client/server layout")
    lo, hi = map(int, Path("/proc/sys/net/ipv4/ip_local_port_range").read_text().split())
    args.tuples = n * args.aliases * (hi - lo + 1)
    if args.users > args.tuples:
        raise ValueError("insufficient native source tuples")
    if not 256 <= args.memory_mib <= 2048 - 8 * n:
        raise ValueError("native scope plus bounds on private helper memory must fit in 2 GiB")
    args.out.mkdir(mode=0o700, parents=True)
    args.out = args.out.resolve()
    args.work = args.work.resolve()
    # A short private control path fits sockaddr_un regardless of checkout depth.
    # Keep the physical database inside the requested disk-backed work directory.
    with tempfile.TemporaryDirectory(prefix="twelve-veth-") as td, tempfile.TemporaryDirectory(
        dir=args.work, prefix="veth-db-"
    ) as db:
        args.db = Path(db) / "feed.db"
        with PrivatePairs(
            Path(td),
            args.out,
            args.server_cpu,
            args.groups,
            args.aliases,
            args.work,
            args.tokens.resolve(),
            args.manager_cpu,
            args.targets_per_client,
        ) as pairs:
            verified = launch(args, pairs, Path(td))
    # Place the nonzero exit after both temporary directories, every endpoint,
    # the native scope and its journal have all completed their cleanup.
    if args.verify_close and not verified:
        raise SystemExit("post-close verification failed; see post-close.json")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--verify-close",
        action="store_true",
        help="Observe native cgroup memory through all peer closes, then check each real HTTP listener and the durable SQLite database.",
    )
    p.add_argument("--server", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--seed", type=Path, required=True)
    p.add_argument("--tokens", type=Path, required=True)
    p.add_argument("--manager-cpu", type=int, required=True)
    p.add_argument("--secret", default="twelve-dollar-challenge")
    p.add_argument("--users", type=int, required=True)
    p.add_argument("--ramp", type=int, default=60)
    p.add_argument("--hold", type=int, default=300)
    p.add_argument("--sample", type=float, default=2)
    p.add_argument(
        "--groups",
        type=lambda v: [[int(c) for c in g.split(",")] for g in v.split(";")],
        required=True,
        help="disjoint client CPU groups separated by semicolons, e.g. 0,1;2,3",
    )
    p.add_argument("--server-cpu", type=int, required=True)
    p.add_argument("--memory-mib", type=int, default=1750)
    p.add_argument("--aliases", type=int, default=12)
    p.add_argument("--targets-per-client", type=int, default=2)
    p.add_argument(
        "--drain-timeout", type=float, default=120, help="maximum seconds for post-score TCP drain"
    )
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    main(p.parse_args())
