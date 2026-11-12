"""Run the unchanged organizer client, measuring both the application and the physical core."""

from contextlib import ExitStack
from contextlib import closing
import json
import os
from pathlib import Path
import re
import signal
import sqlite3
import tempfile
import time

from common import core_counts, docker, group_counts, group_of, subnet_request, tcp_sockstat
from closure import CloseObservation, process_alive
from clients import counters, prepare, read_results, start_workers
from lab import PORT, application, digest, health, save


def kv(provider, op):
    return {
        key: int(value)
        for key, value in (field.split("=") for field in subnet_request(provider, op).split())
    }


def stack_stats(pid, log):
    pos = log.stat().st_size
    os.kill(pid, signal.SIGUSR1)
    until = time.monotonic() + 1
    with log.open("rb") as f:
        f.seek(pos)
        remainder = b""
        while time.monotonic() < until:
            pending = remainder + f.read()
            *complete, remainder = pending.split(b"\n")
            for line in complete:
                if line.startswith(b"bypass stats "):
                    return {k: int(v) for k, v in (x.split("=") for x in line.decode().split()[2:])}
            time.sleep(0.005)
    raise TimeoutError("the bypass process did not report its own TCP/HTTP counters in this interval")


def private_conntrack(provider):
    value = subnet_request(provider, b"?").strip()
    return int(value) if value.isdecimal() else None


def redirect_cpus(path):
    for line in reversed(path.read_text().splitlines()):
        match = re.fullmatch(r"xdp redirect_cpus=([\d,:]*) total=(\d+)", line)
        if match and int(match[2]):
            counts = {
                int(key): int(value) for key, value in (part.split(":") for part in match[1].split(","))
            }
            if sum(counts.values()) != int(match[2]):
                raise ValueError("XDP CPU counters do not sum to their reported total")
            return counts
    return {}


def compatible_stack_stats(pid, log, scope):
    if not scope.exists():
        return None
    try:
        return stack_stats(pid, log)
    except (ProcessLookupError, TimeoutError):
        return None


def measure_window(samples, begin, end, requests, cpu):
    a, b = samples[begin], samples[end]
    dt = (b["time_ns"] - a["time_ns"]) / 1e9
    core = f"cpu{cpu}"
    hz = os.sysconf("SC_CLK_TCK")
    c = {
        k: (b["cores"][core][k] - a["cores"][core][k]) / hz
        for k in ("user", "nice", "sys", "idle", "iowait", "irq", "softirq", "steal")
    }
    busy = sum(c[k] for k in ("user", "nice", "sys", "irq", "softirq"))
    client_states = {
        k: sum(b["cores"][n][k] - a["cores"][n][k] for n in a["cores"] if n != core) / hz for k in c
    }
    client_busy = sum(client_states[k] for k in ("user", "nice", "sys", "irq", "softirq"))

    def used(group):
        if "usage_usec" in b[group] and "usage_usec" in a[group]:
            return (b[group]["usage_usec"] - a[group]["usage_usec"]) / 1e6
        return None

    result = {
        "begin_s": begin,
        "end_s": end,
        "sampled_wall_s": dt,
        "completed_requests": requests,
        "physical_core_busy_s": busy,
        "physical_core_fraction": busy / dt,
        "physical_core_by_state_s": c,
        "client_physical_cores_busy_s": client_busy,
        "client_physical_cores_by_state_s": client_states,
        "application_cgroup_cpu_s": used("application"),
        "server_ancillary_cgroup_cpu_s": used("anchor") + used("broker") + (used("bridge") if "bridge" in a else 0),
        "client_cgroup_cpu_s": used("client"),
    }
    if requests:
        result["physical_core_cpu_us_per_request"] = busy * 1e6 / requests
        result["combined_server_client_cpu_us_per_request"] = (busy + client_busy) * 1e6 / requests
        if used("application") is not None:
            result["application_cpu_us_per_request"] = used("application") * 1e6 / requests
    return result


def score(args, link):
    if args.users <= 0 or args.ramp < 1 or args.hold < 1 or args.think <= 0:
        raise ValueError("users, ramp, hold and think scale must be positive")
    workers = prepare(args, link)
    bridge = getattr(link, "bridge_group", None)
    hashes = {
        k: digest(path)
        for k, path in (
            ("server_sha256", args.binary),
            ("scorer_sha256", args.work / "mload"),
            ("seed_sha256", args.seed),
            ("tokens_sha256", args.tokens),
        )
    }
    with closing(sqlite3.connect(args.seed.as_uri() + "?mode=ro", uri=True)) as conn:
        seed_posts = conn.execute("SELECT COUNT(*) FROM posts").fetchone()[0]
    with tempfile.TemporaryDirectory(dir=link.root) as tmp, ExitStack() as verification:
        db = Path(tmp) / "feed.db"
        with application(
            args, link, db, "service", "twelve-dollar-challenge", args.seed
        ) as api, ExitStack() as observation:
            target, pid, scope, broker, out = api
            anchor = group_of(int(docker("inspect", "--format", "{{.State.Pid}}", link.server_ids[0])))
            runtime = json.loads((out / "runtime.json").read_text())
            if (bridge and args.mode == "xdp" and args.xdp_attachment == "auto" and
                    "mode=AF_XDP/driver " not in runtime["link"].get("transport", "")):
                raise RuntimeError("the bridged native XDP measurement requires an actual driver attachment")
            if bridge:
                from native_bridge import verify_replies

                verify_replies(link, target, PORT)
            duration = args.ramp + args.hold
            start = start_workers(args, link, workers, target, PORT)
            pidfd = os.pidfd_open(pid)
            observation.callback(os.close, pidfd)
            monitor = CloseObservation(
                scope,
                db,
                pidfd,
                start + duration * 1000,
                args.out,
                (link.providers, link.client_providers),
            )
            observation.callback(monitor.stop)
            plan = {
                "mode": args.mode,
                "users": args.users,
                "ramp_s": args.ramp,
                "hold_s": args.hold,
                "think_scale": args.think,
                "scored_start_unix_ms": start,
                **hashes,
                "groups": link.groups,
                "shards": [
                    {k: w[k] for k in ("uid", "address", "cpus", "users", "offset")} for w in workers
                ],
                "source_port_capacity": getattr(link, "source_limits", None),
                "server_cpu": args.server_cpu,
                "manager_cpu": args.manager_cpu,
                "original_application_pid": pid,
                "application_cgroup": str(scope),
                "application_limit_mib": args.memory_mib,
                "requested_application_limit_mib": getattr(args, "requested_memory_mib", args.memory_mib),
                "server_anchor_limit_mib": 8,
                "bridge_limit_mib": 8 if bridge else 0,
                "broker_limit_mib": 12 if broker else 0,
                "total_server_memory_limit_mib": args.memory_mib + 8 + (8 if bridge else 0) + (12 if broker else 0),
                "server_network_namespace": runtime["link"],
                "server_capabilities": 0,
                "hard_nofile": 65535,
                "server_rps_cpus": [args.server_cpu],
                "client_rps_cpus": link.groups[0],
                "target": target,
                "port": PORT,
                "client_wire_checksums": (args.out / "client-wire-checksums.json").exists(),
                "native_bridge": bool(bridge),
                "shared_ip": args.shared_ip,
                "xdp_attachment": args.xdp_attachment if args.mode == "xdp" else None,
                "xdp_redirect_cpu_counters": args.rx_cpu_stats,
                "packet_rx_frames": args.packet_rx_frames,
                "packet_tx_frames": getattr(args, "packet_tx_frames", None),
                "xdp_rx_frames": args.xdp_rx_frames,
                "xdp_tx_frames": args.xdp_tx_frames,
                "packet_gso": args.packet_gso,
            }
            save(args.out / "plan.json", plan)
            print("scoring", json.dumps(plan), flush=True)

            def snapshot(t):
                sample = {
                    "t": t,
                    "time_ns": time.time_ns(),
                    "application": group_counts(scope),
                    "anchor": group_counts(anchor),
                    "broker": group_counts(broker) if broker else {"usage_usec": 0, "memory.current": 0},
                    "client": counters(workers),
                    "cores": core_counts([args.server_cpu, *link.groups[0]]),
                    "server_tcp": link.server_counters(),
                    "client_tcp": kv(link.client_providers[0], b"T"),
                    "server_link": kv(link.providers[0], b"I"),
                    "client_link": kv(link.client_providers[0], b"I"),
                    "private_conntrack": {
                        "server": private_conntrack(link.providers[0]),
                        "client": private_conntrack(link.client_providers[0]),
                    },
                }
                if bridge:
                    sample["bridge"] = group_counts(bridge)
                    if "usage_usec" not in sample["bridge"]:
                        raise RuntimeError("the private bridge's budgeted cgroup vanished")
                    sample["bridge_links"] = {
                        "client_facing": kv(link.bridge_provider, b"0"),
                        "server_facing": kv(link.bridge_provider, b"1"),
                    }
                    sample["private_conntrack"]["bridge"] = private_conntrack(link.bridge_provider)
                return sample

            samples = {}
            with (args.out / "samples.jsonl").open("w") as rows:
                for sec in range(duration + 1):
                    wait = start / 1000 + sec - time.time()
                    if wait > 0:
                        time.sleep(wait)
                    row = snapshot(sec)
                    samples[sec] = row
                    print(json.dumps(row), file=rows, flush=True)
                    if args.mode != "baseline" and sec in (0, args.ramp + 2, duration - 1):
                        print(
                            json.dumps(
                                {
                                    "t": sec,
                                    "stack": compatible_stack_stats(pid, out / "server.log", scope),
                                }
                            ),
                            file=rows,
                            flush=True,
                        )
            report, ticks = read_results(args, workers)
            before, after = samples[0]["server_tcp"], samples[duration]["server_tcp"]
            kernel_tcp = {k: after[k] - before[k] for k in before if k != "CurrEstab"}
            peak_client = max(s["client_tcp"]["CurrEstab"] for s in samples.values())
            peak_kernel = max(s["server_tcp"]["CurrEstab"] for s in samples.values())
            ticks = [row for row in ticks if row["t"] < duration]
            all_open = [row["t"] for row in ticks if row["conns"] == args.users]
            summary = {
                "stock": {
                    k: report[k]
                    for k in (
                        "users",
                        "dials",
                        "reqs",
                        "req_per_s",
                        "fails",
                        "err_pct",
                        "p50_ms",
                        "p95_ms",
                        "p99_ms",
                        "max_ms",
                        "pass",
                        "errors",
                    )
                },
                "server_kernel_tcp_delta": kernel_tcp,
                "peak_client_tcp": peak_client,
                "peak_kernel_server_tcp": peak_kernel,
                "full_window": measure_window(samples, 0, duration, report["reqs"], args.server_cpu),
                "application_scored_peak": max(
                    z["application"].get("memory.peak", 0) for z in samples.values()
                ),
                "scored_peak_total_server_memory": max(
                    sum(z.get(g, {}).get("memory.current", 0) for g in ("application", "anchor", "broker", "bridge"))
                    for z in samples.values()
                ),
                "peak_private_conntrack": {
                    role: max(
                        (
                            v["private_conntrack"][role]
                            for v in samples.values()
                            if v["private_conntrack"][role] is not None
                        ),
                        default=None,
                    )
                    for role in samples[0]["private_conntrack"]
                },
                "stock_peak_connections": max((t["conns"] for t in ticks), default=0),
            }
            successes = report["reqs"] - report["fails"]
            summary["stock"]["successful_requests"] = successes
            summary["stock"]["successful_requests_per_s"] = successes / report["duration_s"]
            if successes:
                window = summary["full_window"]
                window["physical_core_us_per_success"] = 1e6 * window["physical_core_busy_s"] / successes
            if bridge:
                first, last = samples[0], samples[duration]
                summary["private_bridge"] = {
                    "cgroup_cpu_s": (last["bridge"]["usage_usec"] - first["bridge"]["usage_usec"]) / 1e6,
                    "cgroup_scored_peak_bytes": max(s["bridge"]["memory.peak"] for s in samples.values()),
                    "port_counter_deltas": {
                        role: {k: val - first["bridge_links"][role][k] for k, val in after.items()}
                        for role, after in last["bridge_links"].items()
                    },
                }
            if len(all_open) >= 2 and all(
                t["conns"] == args.users for t in ticks if all_open[0] <= t["t"] <= all_open[-1]
            ):
                n = sum(t["reqs"] for t in ticks if all_open[0] < t["t"] <= all_open[-1])
                summary["all_open_window"] = measure_window(
                    samples, all_open[0], all_open[-1], n, args.server_cpu
                )
            if args.mode != "baseline":
                summary["stack_after_load"] = compatible_stack_stats(pid, out / "server.log", scope)
                summary["tcp_bypass_verified"] = (
                    not peak_kernel and not kernel_tcp["InSegs"] and not kernel_tcp["OutSegs"]
                )
            save(args.out / "scored-summary.json", summary)
            close_start = time.monotonic()
            with (args.out / "after-close.jsonl").open("w") as closure_log:
                while True:
                    elapsed = time.monotonic() - close_start
                    client_tcp = kv(link.client_providers[0], b"T")
                    server_tcp = link.server_counters()
                    c, k = client_tcp["CurrEstab"], server_tcp["CurrEstab"]
                    client_sockstat = tcp_sockstat(link.client_providers[0])
                    stack = (
                        compatible_stack_stats(pid, out / "server.log", scope)
                        if args.mode != "baseline"
                        else None
                    )
                    print(
                        json.dumps(
                            {
                                "elapsed_s": elapsed,
                                "client_tcp": client_tcp,
                                "server_tcp": server_tcp,
                                "client_sockstat": client_sockstat,
                                "stack": stack,
                            }
                        ),
                        file=closure_log,
                        flush=True,
                    )
                    confirmed_stack_empty = args.mode == "baseline" or (
                        stack is not None and stack["live"] == 0
                    )
                    if elapsed >= args.close_timeout or (c == 0 and k == 0 and client_sockstat["inuse"] == 0 and
                            (confirmed_stack_empty or not process_alive(pidfd))):
                        break
                    time.sleep(0.25 if elapsed < 5 else 1)
            summary["close"] = {
                "remaining_client": c,
                "remaining_client_sockets": client_sockstat["inuse"],
                "remaining_server_kernel": k,
                "stack": stack,
                "duration_s": time.monotonic() - close_start,
                "original_pid_alive": process_alive(pidfd),
                "database_health": health(link, target),
            }
            save(args.out / "post-close.json", summary["close"])
            # Verify the original database before the temporary directory is removed,
            # including when disposing of an already-stopped container times out.
            verification.callback(finish_report, args, db, out, seed_posts, report, summary, bool(bridge))
            for worker in workers:
                (worker["output"] / "release").touch()


def finish_report(args, db, out, seed_posts, report, summary, bridge):
    closure = [json.loads(line) for line in (args.out / "closure.jsonl").read_text().splitlines()]
    summary["lifetime"] = {
        "application_peak_bytes": max((s.get("memory.peak", 0) for s in closure), default=0),
        "max_oom_kill": max(
            (s.get("memory.events", {}).get("oom_kill", 0) for s in closure), default=0
        ),
        "original_pid_alive_through_last_probe": all(s["native_alive"] for s in closure) if closure else None,
    }
    if args.rx_cpu_stats:
        summary["xdp_redirects_by_cpu"] = redirect_cpus(out / "server.log")
    with closing(sqlite3.connect(db.as_uri() + "?mode=ro", uri=True)) as conn:
        rows = {
            table: conn.execute(f"SELECT count(*) FROM {table}").fetchone()[0]
            for table in ("users", "posts", "likes")
        }
        checks = conn.execute("PRAGMA quick_check").fetchall()
    acknowledged = next(r["ok"] for r in report["endpoints"] if r["name"] == "POST /posts")
    summary["persisted"] = {
        "rows": rows,
        "acknowledged_post_creations": acknowledged,
        "seed_post_rows": seed_posts,
        "new_post_rows": rows["posts"] - seed_posts,
        "quick_check": [row[0] for row in checks],
    }
    summary["persisted"]["durable_post_count_covers_acknowledged_creates"] = (
        rows["posts"] - seed_posts >= acknowledged
    )
    save(args.out / "summary.json", summary)
    print(json.dumps(summary), flush=True)
    if (
        not summary["close"]["database_health"]
        or checks != [("ok",)]
        or not summary["persisted"]["durable_post_count_covers_acknowledged_creates"]
    ):
        raise RuntimeError("post-workload health or durable database check failed")
    if args.mode != "baseline" and not summary["tcp_bypass_verified"]:
        raise RuntimeError("the native Linux TCP path handled userspace-stack traffic")
    if args.rx_cpu_stats and not summary["xdp_redirects_by_cpu"]:
        raise RuntimeError("no XDP redirect placement was observed")
    if (
        args.rx_cpu_stats
        and (args.xdp_attachment == "skb" or bridge)
        and set(summary["xdp_redirects_by_cpu"]) != {args.server_cpu}
    ):
        raise RuntimeError("XDP redirects were observed outside the measured server CPU")
    close = summary["close"]
    stack = close["stack"]
    remaining_peers = any(close[k] for k in ("remaining_client", "remaining_server_kernel", "remaining_client_sockets"))
    stack_busy = args.mode != "baseline" and (stack is None or stack["live"] or stack["buffered"])
    if remaining_peers or stack_busy:
        raise RuntimeError(
            "some private peers or application buffers remained after the disconnect window"
        )
