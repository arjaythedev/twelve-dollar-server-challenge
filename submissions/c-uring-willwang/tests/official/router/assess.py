"""Recompute stock eligibility and generator headroom from preserved shard JSON and host samples."""

import json
import math
from pathlib import Path
import re
import sys


def read_json(path):
    return json.loads(path.read_text())


def rows(path):
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def percentile(data, q):
    return round(sorted(data)[max(0, math.ceil(len(data) * q) - 1)], 2) if data else None


def scheduler_accounting(app_rows, cpu_rows, cpu, start, end):
    """Report cgroup, per-CPU and NAPI clocks with their actual sampled intervals."""
    app = [r for r in app_rows if start <= r["unix"] < end and "cpu.stat" in r.get("app", {})]
    cpu_key = f"cpu{cpu}"
    core = [r for r in cpu_rows if start <= r["unix"] < end and cpu_key in r.get("cpus", {})]
    if len(app) < 2 or len(core) < 2:
        return {}

    def time_range(points):
        return {
            "start_unix": points[0]["unix"],
            "end_unix": points[-1]["unix"],
            "coverage_s": round(points[-1]["unix"] - points[0]["unix"], 4),
        }

    start_time, end_time = app[0]["app"]["cpu.stat"], app[-1]["app"]["cpu.stat"]
    elapsed = app[-1]["unix"] - app[0]["unix"]
    usage = {
        key: end_time[key] - start_time[key]
        for key in ("usage_usec", "user_usec", "system_usec")
        if key in start_time and key in end_time
    }
    out = {
        "application_cgroup": {
            **time_range(app),
            **usage,
            "one_cpu_pct": round(usage["usage_usec"] / (elapsed * 10000), 3),
        }
    }
    first, last = core[0], core[-1]
    ticks = {key: value - first["cpus"][cpu_key][key] for key, value in last["cpus"][cpu_key].items()}
    total = sum(ticks.values())
    out["application_cpu"] = {
        **time_range(core),
        "cpu": cpu,
        "pct_by_category": (
            {key: round(value * 100 / total, 3) for key, value in ticks.items()} if total else {}
        ),
    }
    napi = [r for r in core if r.get("napi_threads")]
    if len(napi) >= 2:
        first, last = napi[0], napi[-1]
        elapsed = last["unix"] - first["unix"]
        threads = {}
        for pid, before in first["napi_threads"].items():
            after = last["napi_threads"].get(pid)
            if after is None:
                continue
            run = after["run_ns"] - before["run_ns"]
            wait = after["wait_ns"] - before["wait_ns"]
            slices = after["timeslices"] - before["timeslices"]
            threads[pid] = {
                "run_s": round(run / 1e9, 4),
                "runnable_wait_s": round(wait / 1e9, 4),
                "timeslices": slices,
                "one_cpu_pct": round(run / (elapsed * 1e7), 3),
                "mean_run_us_per_timeslice": round(run / (1000 * slices), 3) if slices else None,
            }
        out["private_napi"] = {**time_range(napi), "threads": threads}
    return out


def cpu_windows(data, ids, start, end):
    ids = [f"cpu{cpu}" for cpu in ids]
    data = [row for row in data if start <= row["unix"] <= end and all(i in row["cpus"] for i in ids)]
    if len(data) < 2:
        return {}

    def unit(a, b):
        d = {k: sum(b["cpus"][i][k] - a["cpus"][i][k] for i in ids) for k in a["cpus"][ids[0]]}
        n = sum(d.values())
        return {k: 100 * v / n for k, v in d.items()} if n else {}

    windows = [unit(a, b) for a, b in zip(data, data[1:])]
    means = unit(data[0], data[-1])
    busy = [100 - delta["idle"] - delta.get("iowait", 0) for delta in windows if delta]
    start_idx = 0
    max_busy_30s = 0
    for i, last in enumerate(data):
        while start_idx + 1 < i and last["unix"] - data[start_idx + 1]["unix"] >= 30:
            start_idx += 1
        if last["unix"] - data[start_idx]["unix"] >= 30:
            value = unit(data[start_idx], last)
            max_busy_30s = max(max_busy_30s, 100 - value["idle"] - value.get("iowait", 0))
    return {
        "coverage_s": round(data[-1]["unix"] - data[0]["unix"], 1),
        "mean_busy_pct": round(100 - means["idle"] - means.get("iowait", 0), 2),
        "p95_interval_busy_pct": percentile(busy, 0.95),
        "max_30s_busy_pct": round(max_busy_30s, 2),
        "mean_softirq_pct": round(means["softirq"], 2),
        "mean_steal_pct": round(means["steal"], 2),
    }


def split_worker_schedule(samples, workers, start, hold_start, end):
    """Measure time each dedicated polling worker spent running and waiting to run."""
    result, reasons = {}, []
    complete = bool(workers)
    for stage, low, high in (("ramp", start, hold_start), ("hold", hold_start, end)):
        points = [s for s in samples if low <= s["unix"] < high]
        stage_rows = result[stage] = {}
        for worker in workers:
            cpu, tid = worker["cpu"], str(worker["tid"])
            elapsed = running = waiting = longest = streak = 0.0
            observed = 0
            switches = 0
            for left, right in zip(points, points[1:]):
                first = left.get("relay_worker_scheduling", {}).get(tid)
                last = right.get("relay_worker_scheduling", {}).get(tid)
                dt = right["unix"] - left["unix"]
                if not first or not last or dt <= 0:
                    streak = 0.0
                    continue
                run = last["run_ns"] - first["run_ns"]
                wait = last["runqueue_wait_ns"] - first["runqueue_wait_ns"]
                slices = last["timeslices"] - first["timeslices"]
                if min(run, wait, slices) < 0:
                    streak = 0.0
                    continue
                observed += 1
                elapsed += dt
                running += run
                waiting += wait
                switches += slices
                streak = streak + dt if wait >= 0.2 * dt * 1e9 else 0.0
                longest = max(longest, streak)
            enough = observed > 0 and observed == len(points) - 1
            complete = complete and enough
            stage_rows[str(cpu)] = {
                "tid": int(tid), "role": worker["role"],
                "observed_intervals": observed, "complete_samples": enough,
                "coverage_s": round(elapsed, 3), "timeslices": switches,
                "running_pct": round(running / elapsed / 1e7, 3) if elapsed else None,
                "runnable_wait_pct": round(waiting / elapsed / 1e7, 3) if elapsed else None,
                "consecutive_20pct_wait_s": round(longest, 3),
            }
            if longest >= 30:
                reasons.append(f"dedicated relay CPU {cpu} waited to run for at least 20% of each sampled "
                               f"interval for at least 30 consecutive seconds during {stage}")
    return result, reasons, complete


def polling_worker_headroom(samples, config, start, ramp, end):
    expected = {w["tid"]: w for w in config["verified_workers"]}
    health = {"ramp": {}, "hold": {}}
    reasons = set()
    valid = True
    for stage, low, high in (("ramp", start, start + ramp), ("hold", start + ramp, end)):
        pairs = [(a, b) for a, b in zip(samples, samples[1:]) if low <= b["unix"] < high]
        for tid, identity in expected.items():
            windows = []
            fifo_stats = {}
            busy_since = None
            long_busy = 0
            for a, b in pairs:
                earlier = next((w for w in a["polling_relay"].get("workers", []) if w["tid"] == tid), None)
                later = next((w for w in b["polling_relay"].get("workers", []) if w["tid"] == tid), None)
                if not earlier or not later or later["cpu"] != identity["cpu"]:
                    valid = False
                    continue
                loops = later["loops"] - earlier["loops"]
                if loops <= 0:
                    valid = False
                    continue
                frames = later["forwarded_events"] - earlier["forwarded_events"]
                idle = 100 * (later["idle_loops"] - earlier["idle_loops"]) / loops
                late = 100 * (later["late_over_250us"] - earlier["late_over_250us"]) / frames if frames else 0
                elapsed_ns = later.get("snapshot_monotonic_ns", 0) - earlier.get("snapshot_monotonic_ns", 0)
                dt = elapsed_ns / 1e9 if elapsed_ns > 0 else b["unix"] - a["unix"]
                productive = later.get("busy_ns", 0) - earlier.get("busy_ns", 0)
                idle_time = later.get("idle_ns", 0) - earlier.get("idle_ns", 0)
                previous_fifos = {row["source"]: row for row in earlier.get("fifos", [])}
                for fifo in later.get("fifos", []):
                    previous = previous_fifos.get(fifo["source"], {})
                    record = fifo_stats.setdefault(str(fifo["source"]), {})
                    for key in (
                        "full_polls",
                        "write_retries",
                        "retry_expired",
                        "forwarded",
                        "read_burst_full",
                        "send_burst_full",
                    ):
                        if key in fifo and key in previous:
                            record[key] = record.get(key, 0) + fifo[key] - previous[key]
                    record["maximum_sampled_depth"] = max(
                        record.get("maximum_sampled_depth", 0), fifo.get("queue_depth", 0)
                    )
                    record["cumulative_peak_at_stage_end"] = fifo.get("queue_peak", 0)
                windows.append(
                    (
                        idle,
                        late,
                        frames / dt,
                        100 * (later["thread_cpu_ns"] - earlier["thread_cpu_ns"]) / (dt * 1e9),
                        100 * idle_time / (productive + idle_time) if productive + idle_time else 0,
                    )
                )
                if idle < 1:
                    if busy_since is None:
                        busy_since = a["unix"]
                    long_busy = max(long_busy, b["unix"] - busy_since)
                    if late > 1:
                        reasons.add(
                            f"polling worker {identity['role']} on CPU {identity['cpu']} exhausted scan headroom and missed wire deadlines during {stage}"
                        )
                else:
                    busy_since = None
            if not windows:
                valid = False
                continue
            if long_busy >= 30:
                reasons.add(
                    f"polling worker {identity['role']} on CPU {identity['cpu']} had under 1% idle scans for 30 seconds during {stage}"
                )
            health[stage][str(tid)] = {
                **identity,
                "sample_windows": len(windows),
                "minimum_idle_scan_pct": round(min(w[0] for w in windows), 3),
                "maximum_over_250us_late_pct": round(max(w[1] for w in windows), 3),
                "maximum_forwarded_events_s": round(max(w[2] for w in windows)),
                "maximum_thread_cpu_pct": round(max(w[3] for w in windows), 2),
                "minimum_idle_wall_time_pct": round(min(w[4] for w in windows), 2),
                "longest_below_1pct_idle_s": round(long_busy, 2),
                "source_fifos": fifo_stats,
            }
    return health, sorted(reasons), valid and bool(expected)


def split_handoff_intervals(samples, config, start, ramp, end):
    result, reasons = {}, set()
    valid = True
    for stage, low, high in (("ramp", start, start + ramp), ("hold", start + ramp, end)):
        out = {}
        pairs = [(a, b) for a, b in zip(samples, samples[1:]) if low <= b["unix"] < high]
        for identity in config["verified_handoffs"]:
            dest = identity["destination"]
            record = {
                **identity,
                "windows": 0,
                "max_sampled_depth": 0,
                "max_interval_late_over_250us_pct": 0,
                "producer_wait_hist": [0] * 6,
                "delivery_late_hist": [0] * 6,
            }
            for a, b in pairs:
                left = next(
                    (h for h in a["polling_relay"].get("handoffs", []) if h["destination"] == dest), None
                )
                right = next(
                    (h for h in b["polling_relay"].get("handoffs", []) if h["destination"] == dest), None
                )
                if (
                    not left
                    or not right
                    or right["producer_cpu"] != identity["producer_cpu"]
                    or right["consumer_cpu"] != identity["consumer_cpu"]
                ):
                    valid = False
                    continue
                record["windows"] += 1
                for key in (
                    "enqueued",
                    "delivered",
                    "written",
                    "dropped",
                    "full_polls",
                    "write_retries",
                    "late_over_50us",
                    "late_over_250us",
                    "late_over_1000us",
                ):
                    record[key] = record.get(key, 0) + right[key] - left[key]
                for key in ("producer_wait_hist", "delivery_late_hist"):
                    if len(left[key]) != 6 or len(right[key]) != 6:
                        valid = False
                        continue
                    record[key] = [r + y - x for r, x, y in zip(record[key], left[key], right[key])]
                sent = right["delivered"] - left["delivered"]
                late = right["late_over_250us"] - left["late_over_250us"]
                record["max_interval_late_over_250us_pct"] = max(
                    record["max_interval_late_over_250us_pct"], round(100 * late / sent, 3) if sent else 0
                )
                record["max_sampled_depth"] = max(record["max_sampled_depth"], right["queue_depth"])
                record["cumulative_peak_at_stage_end"] = right["queue_peak"]
            if not record["windows"]:
                valid = False
            if record.get("dropped", 0):
                reasons.add(f"private reply handoff to generator TAP {dest} dropped frames during {stage}")
            out[str(dest)] = record
        result[stage] = out
    return result, sorted(reasons), valid and all(len(r) == 4 for r in result.values())


def assess(out):
    shape, base = read_json(out / "shape.json"), read_json(out / "summary.json")
    raw = [read_json(path) for path in sorted(out.glob("shard-*.json"))]
    if not raw:
        raise ValueError("no complete stock shard results")
    ramp, hold, target = shape["ramp_s"], shape["hold_s"], shape["users"]
    first_tick = math.ceil((ramp + 5) / 5) * 5
    expected = list(range(first_tick, ramp + hold, 5))

    # Each stock mload user waits a random 0–5 s before its first GET.
    # Every expected simultaneous tick after that period must actually exist.
    def ticks(row):
        output = {}
        for t in row["timeline"]:
            nearest = 5 * round(t["t"] / 5)
            if abs(t["t"] - nearest) <= 1:
                output[nearest] = t
        return output

    by_shard = [ticks(r) for r in raw]
    aligned = []
    for t in expected:
        if all(t in shard for shard in by_shard):
            aligned.append(
                {
                    "t": t,
                    "users": sum(row[t]["users"] for row in by_shard),
                    "conns": sum(row[t]["conns"] for row in by_shard),
                }
            )
    full = [v for v in aligned if v["users"] == target and v["conns"] == target]
    concurrent = {
        "expected_5s_ticks": len(expected),
        "synchronized_ticks": len(aligned),
        "first_observed_s": aligned[0]["t"] if aligned else None,
        "last_observed_s": aligned[-1]["t"] if aligned else None,
        "min_users": min((r["users"] for r in aligned), default=0),
        "min_established": min((r["conns"] for r in aligned), default=0),
        "max_established": max((r["conns"] for r in aligned), default=0),
        "full_ticks": len(full),
        "all_expected_full": bool(expected and len(full) == len(expected)),
    }
    samples, measured = rows(out / "samples.jsonl"), rows(out / "telemetry.jsonl")
    timing = (
        read_json(out / "timing.json")
        if (out / "timing.json").exists()
        else {
            "stock_start_unix": samples[0]["unix"] + 4,
            "cutoff_unix": samples[0]["unix"] + 4 + ramp + hold,
            "inferred_old_run": True,
        }
    )
    start = timing["stock_start_unix"]
    end = timing["cutoff_unix"]
    app_cpu = shape["app_cpu"]
    router_cpus = shape["router_cpus"] if "router_cpus" in shape else [shape["router_cpu"]]
    relay_config = shape.get("network", {}).get(shape["namespaces"][1], {}).get("polling_relay", {})

    def cpulist(group):
        return [
            int(c)
            for part in group.split(",")
            for c in (range(int(part.split("-")[0]), int(part.split("-")[1]) + 1) if "-" in part else [part])
        ]

    groups = [cpulist(s) for s in shape["client_cpus"]]
    coverage = {}
    for stage, limits in {"ramp": (start, start + ramp), "hold": (start + ramp, end)}.items():
        cpu_data = sorted([*samples, *measured], key=lambda s: s["unix"]) if not measured else measured
        coverage[stage] = {
            "app": cpu_windows(cpu_data, [app_cpu], *limits),
            "router_by_cpu": {str(c): cpu_windows(cpu_data, [c], *limits) for c in router_cpus},
            "clients": [cpu_windows(cpu_data, g, *limits) for g in groups],
        }
    router_file = out / "router-queues.json"
    queues = {
        dev: int(m.group(1))
        for dev, txt in (read_json(router_file) if router_file.exists() else {}).items()
        if (m := re.search(r"\(dropped (\d+)", txt))
    }
    queue_samples = [s for s in rows(out / "network-samples.jsonl") if s["unix"] < end]
    scored_queues = (
        {dev: data["drops"] for dev, data in queue_samples[-1]["router_queues"].items()}
        if queue_samples
        else queues
    )
    scored_router_rx = queue_samples[-1].get("router_rx", {}) if queue_samples else {}
    pre = [s for s in measured if start <= s["unix"] < end]
    active = [s for s in pre if s["unix"] >= start + ramp + 5]
    first, last = (pre[0], pre[-1]) if pre else ({}, {})

    def deltas(key):
        return {k: last[key][k] - first[key][k] for k in first.get(key, {}) if k in last.get(key, {})}

    tcp = deltas("Tcp")
    tcp.pop("CurrEstab", None)
    kernel = [s["Tcp"]["CurrEstab"] for s in active if "Tcp" in s and "CurrEstab" in s["Tcp"]]
    rtt = {}
    for path in sorted(out.glob("rtt-*.log")):
        parsed = [
            (float(m.group(1)), float(m.group(2)))
            for line in path.read_text().splitlines()
            if (m := re.search(r"\[([\d.]+)\].*time[=<]([\d.]+) ms", line))
        ]
        rtt[path.stem] = {
            stage: {
                "replies": len(values),
                "p50_ms": percentile(values, 0.50),
                "p95_ms": percentile(values, 0.95),
                "p99_ms": percentile(values, 0.99),
            }
            for stage, lo, hi in (("ramp", start, start + ramp), ("hold", start + ramp, end))
            if (values := [v for ts, v in parsed if lo <= ts < hi])
        }
    interfaces = {}
    for ns_id in shape["namespaces"]:
        path = out / f"interfaces-{ns_id}.json"
        if path.is_file():
            interfaces[ns_id] = {
                r["ifname"]: {
                    d: {
                        k: r.get("stats64", r.get("stats", {})).get(d, {}).get(k, 0)
                        for k in ("bytes", "packets", "errors", "dropped", "missed_errors")
                    }
                    for d in ("rx", "tx")
                }
                for r in read_json(path)
                if r["ifname"] != "lo"
            }
    system = {
        "timing": timing,
        "cpu": coverage,
        "router_qdisc_drops": queues,
        "router_scored_qdisc_drops": scored_queues,
        "router_rx_at_last_scored_sample": scored_router_rx,
        "server_ingress_peer_tx_at_last_scored_sample": {
            dev: stat
            for dev, stat in (queue_samples[-1].get("router_tx", {}) if queue_samples else {}).items()
            if dev.startswith("s")
        },
        "router_last_scored_sample_unix": queue_samples[-1]["unix"] if queue_samples else None,
        "softnet_dropped_by_cpu": (
            {
                str(c): last.get("softnet", {}).get(str(c), {}).get("dropped", 0)
                - first.get("softnet", {}).get(str(c), {}).get("dropped", 0)
                for c in {app_cpu, *router_cpus, *(i for g in groups for i in g)}
            }
            if "softnet" in first
            else {}
        ),
        "tcp_delta": tcp,
        "tcpext_delta": deltas("TcpExt"),
        "memory_stat_first": first.get("memory", {}),
        "rtt_by_generator": rtt,
        "interfaces_at_exit": interfaces,
        "memory_stat_last": last.get("memory", {}),
        "memory_events_delta": deltas("memory_events"),
        "server_kernel_curr_estab_hold_min": min(kernel, default=None),
        "server_kernel_curr_estab_hold_max": max(kernel, default=None),
        "host_sample_duration_s": round(last.get("unix", 0) - first.get("unix", 0), 1),
        "cgroup_start": first.get("service", {}),
        "cgroup_end": last.get("service", {}),
        "pressure_stall_us": {
            kind: {
                tag: last["pressure"][kind][tag]["total"] - first["pressure"][kind][tag]["total"]
                for tag in first["pressure"][kind]
            }
            for kind in first.get("pressure", {})
            if kind in last.get("pressure", {})
        },
    }
    for group in ("diskstats", "io_stat"):
        if group not in first or group not in last:
            continue
        system[group + "_initial"] = first[group]
        system[group + "_delta"] = {
            device: {
                k: value - first[group].get(device, {}).get(k, 0)
                for k, value in counts.items()
                if k != "in_flight"
            }
            for device, counts in last[group].items()
        }
    if "host_io_pressure" in first and "host_io_pressure" in last:
        system["host_io_stall_us"] = {
            kind: last["host_io_pressure"][kind]["total"] - row["total"]
            for kind, row in first["host_io_pressure"].items()
        }
    if "host_sockstat_tcp" in first:
        system["host_sockstat_tcp_initial"] = first["host_sockstat_tcp"]
        system["host_sockstat_tcp_final"] = last.get("host_sockstat_tcp", {})
        system["host_sockstat_orphan_peak"] = max(
            row.get("host_sockstat_tcp", {}).get("orphan", 0) for row in pre
        )
    if first.get("napi_threads") and last.get("napi_threads"):
        seconds = last["unix"] - first["unix"]
        system["private_napi_threads"] = {
            pid: {
                "cpu_pct": round(100 * (now["run_ns"] - before["run_ns"]) / (seconds * 1e9), 2),
                "run_s": round((now["run_ns"] - before["run_ns"]) / 1e9, 4),
                "runnable_wait_s": round((now["wait_ns"] - before["wait_ns"]) / 1e9, 4),
                "timeslices": now["timeslices"] - before["timeslices"],
            }
            for pid, before in first["napi_threads"].items()
            if (now := last["napi_threads"].get(pid))
        }
    system["cpu_accounting_windows"] = {
        label: scheduler_accounting(samples, measured, app_cpu, lo, hi)
        for label, lo, hi in (
            ("ramp", start, start + ramp),
            ("hold", start + ramp, end),
            ("ramp_and_hold", start, end),
        )
    }
    reasons = []
    target_flags = []
    relay_evidence = False
    if relay_config:
        relay_samples = [q for q in queue_samples if q.get("polling_relay")]
        windows = []
        for before, after in zip(relay_samples, relay_samples[1:]):
            a, b = before["polling_relay"], after["polling_relay"]
            loops = b["loops"] - a["loops"]
            frames = b["forwarded_events"] - a["forwarded_events"]
            if loops <= 0 or frames <= 0:
                continue
            windows.append(
                {
                    "start_unix": before["unix"],
                    "end_unix": after["unix"],
                    "idle_scan_pct": 100 * (b["idle_loops"] - a["idle_loops"]) / loops,
                    "over_250us_late_pct": 100 * (b["late_over_250us"] - a["late_over_250us"]) / frames,
                    "frames": frames,
                }
            )
        if windows and all(q["polling_relay"].get("sample_age_s", 3) <= 3 for q in relay_samples):
            relay_evidence = True
        if relay_config.get("verified_workers"):
            worker_health, worker_reasons, worker_evidence = polling_worker_headroom(
                relay_samples, relay_config, start, ramp, end
            )
            system["polling_worker_intervals"] = worker_health
            placed = [s for s in relay_samples if s.get("relay_worker_scheduling")]
            scheduler_expected = relay_config.get("scheduler_observation", {}).get("external_relay_workers")
            if placed or scheduler_expected:
                delivery, contention, complete = split_worker_schedule(
                    relay_samples, relay_config["verified_workers"], start, start + ramp, end
                )
                system["external_worker_cpu_delivery"] = delivery
                reasons.extend(contention)
                if scheduler_expected:
                    worker_evidence = worker_evidence and complete
            if len(placed) >= 2:
                earliest, latest = placed[0]["relay_worker_scheduling"], placed[-1]["relay_worker_scheduling"]
                system["external_worker_schedule_delta"] = {
                    tid: {
                        k: row[k] - prior[k]
                        for k in (
                            "run_ns",
                            "runqueue_wait_ns",
                            "timeslices",
                            "nr_involuntary_switches",
                            "nr_voluntary_switches",
                        )
                        if k in prior and k in row
                    }
                    for tid, prior in earliest.items()
                    if (row := latest.get(tid))
                }
            reasons.extend(worker_reasons)
            relay_evidence = relay_evidence and worker_evidence
        if relay_config.get("verified_handoffs"):
            detail, diagnoses, good = split_handoff_intervals(relay_samples, relay_config, start, ramp, end)
            system["reply_handoff_intervals"] = detail
            system["reply_handoff_histogram_upper_us"] = [10, 50, 100, 250, 1000, "above 1000"]
            reasons.extend(diagnoses)
            relay_evidence = relay_evidence and good
        for name, limits in {"ramp": (start, start + ramp), "hold": (start + ramp, end)}.items():
            stage = [w for w in windows if limits[0] <= w["end_unix"] <= limits[1]]
            if stage:
                system.setdefault("polling_relay_intervals", {})[name] = {
                    "minimum_idle_scan_pct": round(min(w["idle_scan_pct"] for w in stage), 3),
                    "maximum_over_250us_late_pct": round(max(w["over_250us_late_pct"] for w in stage), 3),
                    "windows": len(stage),
                }
                if any(w["idle_scan_pct"] < 1 and w["over_250us_late_pct"] > 1 for w in stage):
                    reasons.append(
                        f"polling wire exhausted its scan headroom and delayed packets during {name}"
                    )
        if relay_samples:
            latest = relay_samples[-1]
            r = latest["polling_relay"]
            system["polling_relay_last_scored"] = r
            system["tap_interfaces_last_scored"] = latest["tap_interfaces"]
            service_ns = shape["namespaces"][0]
            server_directions = latest["tap_interfaces"][service_ns]
            system["tap_drop_attribution"] = {
                "target_receive_interface": server_directions["rx"],
                "target_transmit_interface": server_directions["tx"],
                "relay_queue_overflow": r["queue_overflow"],
                "relay_invalid_frames": r["bad_frame"],
                "relay_write_failed_to_service_cause_unknown": r["ports"][0]["write_drop"],
                "relay_write_failed_to_generators": [port["write_drop"] for port in r["ports"][1:]],
                "generator_interfaces": {
                    ns: counts for ns, counts in latest["tap_interfaces"].items() if ns != service_ns
                },
            }
            if "service_tx_qdiscs" in latest:
                system["tap_drop_attribution"]["service_transmit_qdiscs"] = latest["service_tx_qdiscs"]
                if any(q.get("drops", 0) for q in latest["service_tx_qdiscs"]):
                    reasons.append(
                        "service egress qdisc dropped packets before handing them to the private TAP driver"
                    )
            if r["queue_overflow"] or r["bad_frame"]:
                reasons.append("private polling wire recorded an internal queue overflow or invalid frame")
            if r["ports"][0]["write_drop"]:
                reasons.append(
                    "polling relay failed to inject a frame into the server TAP; cause undetermined"
                )
            if any(port["write_drop"] for port in r["ports"][1:]):
                reasons.append("polling relay failed to deliver frames to generator TAPs")
            for ns, directions in latest["tap_interfaces"].items():
                for side, counts in directions.items():
                    if counts["dropped"] + counts["errors"]:
                        if ns == service_ns and side == "rx":
                            target_flags.append("service TAP receive interface reported a drop or error")
                        else:
                            reasons.append(f"private TAP {side.upper()} in {ns} reported a drop or error")
    if any(scored_queues.values()):
        reasons.append("virtual-router qdisc dropped packets")
    # A router port's TX drop can be the server RX queue refusing a frame.
    # Classify external RX drops through pre-cutoff router data alone.
    if any(cnt.get("dropped", 0) + cnt.get("errors", 0) for cnt in scored_router_rx.values()):
        reasons.append("virtual-router receive interface dropped packets during scoring")
    for stage, info in coverage.items():
        for name, cluster in [
            *[(f"router CPU {c}", v) for c, v in info["router_by_cpu"].items() if not relay_config],
            *[(f"generator {i}", v) for i, v in enumerate(info["clients"])],
        ]:
            if cluster.get("max_30s_busy_pct", 0) >= 98:
                reasons.append(f"{name} averaged at least 98% busy for 30 seconds during {stage}")
    active_router_cpus = relay_config.get("cpus", [relay_config["cpu"]]) if relay_config else router_cpus
    for cpu in active_router_cpus:
        if system["softnet_dropped_by_cpu"].get(str(cpu), 0):
            reasons.append(f"router CPU {cpu} dropped packets in its receive backlog")
    for i, group in enumerate(groups):
        if any(system["softnet_dropped_by_cpu"].get(str(cpu), 0) for cpu in group):
            reasons.append(f"generator {i} CPU receive backlog recorded packet drops")
    wall_times = [r["duration_s"] for r in raw]
    complete = all(ramp + hold - 0.3 <= t < ramp + hold + 2 for t in wall_times)
    raw_eligible = bool(base["users"] == target and not base["oom_kill_at_cutoff"] and complete)
    wire_eligible = raw_eligible and concurrent["all_expected_full"]
    has_external_evidence = all(
        str(cpu) in system["softnet_dropped_by_cpu"] for cpu in active_router_cpus
    ) and (relay_evidence if relay_config else bool(scored_queues))
    infrastructure_ok = has_external_evidence and not reasons
    scheduled_users_present = bool(
        expected and len(aligned) == len(expected) and all(row["users"] == target for row in aligned)
    )
    strictly_eligible = wire_eligible and infrastructure_ok and not target_flags
    final = {
        "stock_pass": base["pass"],
        "stock_schedule_complete": complete,
        "stock_shard_duration_s": wall_times,
        "simultaneous_connections": concurrent,
        "host_checks": system,
        "infrastructure_flags": reasons,
        "target_network_flags": target_flags,
        "captured_host_diagnostics": bool(pre),
        "scored_external_queue_evidence": has_external_evidence,
        "raw_eligible": raw_eligible,
        "all_scheduled_users_present": scheduled_users_present,
        "infrastructure_ok": infrastructure_ok,
        "scored_healthy_environment": raw_eligible and scheduled_users_present and infrastructure_ok,
        "wire_eligible": wire_eligible,
        "eligible": strictly_eligible,
        "qualified": bool(base["pass"] and strictly_eligible),
    }
    base.update(final)
    (out / "synchronized-connections.json").write_text(json.dumps(aligned) + "\n")
    (out / "summary.json").write_text(json.dumps(base, indent=2, sort_keys=True) + "\n")
    return base


if __name__ == "__main__":
    result = assess(Path(sys.argv[1]))
    print(
        json.dumps(
            {
                k: result[k]
                for k in (
                    "tag",
                    "qualified",
                    "stock_pass",
                    "scored_healthy_environment",
                    "wire_eligible",
                    "p99_ms",
                    "infrastructure_flags",
                    "target_network_flags",
                )
            }
        )
    )
