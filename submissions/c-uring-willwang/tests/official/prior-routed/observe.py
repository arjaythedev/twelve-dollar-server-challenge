"""Supplemental private namespace readings and read-only service accounting."""

import json
import os
from pathlib import Path
import re
import re
import sys
import time


def dump(path, data):
    staging = path.with_suffix(path.suffix + ".pending")
    staging.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    staging.replace(path)


def add_placement_checks(module, output):
    original = module.Link.placement

    def observed(link, server_cpu, groups):
        # The historical binary uses its first group and target for each two-sided
        # probe, while accepting replies steered anywhere in the total client mask.
        rows = []
        for lane in range(len(groups)):

            class Lane:
                server, client, router = link.server, link.client, link.router
                targets = link.targets[lane:] + link.targets[:lane]

            if lane == 0:
                data = original(link, server_cpu, groups)
            else:
                data = original(Lane(), server_cpu, groups[lane:] + groups[:lane])
            rows.append(
                {
                    "lane": lane,
                    "target": link.targets[lane][0],
                    "source": f"10.243.{lane+1}.1",
                    "client_process_cpu": min(groups[lane]),
                    **data,
                }
            )
        roots = [link.server, link.client, link.router]
        sysctls = [
            "ipv4/tcp_ehash_entries",
            "ipv4/tcp_child_ehash_entries",
            "ipv4/ip_local_port_range",
            "core/rps_default_mask",
        ]
        files = ["/proc/sys/net/" + k for k in sysctls]
        code = "import json,sys; print(json.dumps([open(p).read().strip() for p in sys.argv[1:]]))"
        names = {
            "manager": {k: Path(p).read_text().strip() for k, p in zip(sysctls, files)}
        }
        for h in roots:
            values = json.loads(
                module.checked(h.cmd([sys.executable, "-c", code, *files]))
            )
            names[h.name] = dict(zip(sysctls, values))
            names[h.name]["inode"] = h.inode
        before = {iface: link.meta[iface + "_qdisc"] for _, iface in link.interfaces}
        result = {
            "lanes": rows,
            "namespaces": names,
            "kernel": os.uname().release,
            "qdisc_before": before,
            "holder_pids": {h.name: h.pid for h in roots},
        }
        dump(output / "placements.json", result)
        return {"server": rows[0]["server"], "client": rows[0]["client"]}

    module.Link.placement = observed


def timed_qdisc_evidence(module, output, phase):
    """Read the isolated qdisc counters without modifying any private or host setting."""
    placed = output / "placements.json"
    raw = output / "raw"
    if not (placed.is_file() and (raw / "runtime.json").is_file()):
        return None
    proof = json.loads(placed.read_text())
    runtime = json.loads((raw / "runtime.json").read_text())
    plan = json.loads((raw / "plan.json").read_text())
    cutoff = runtime["start_unix_ms"] / 1000 + plan["ramp_s"] + plan["hold_s"]
    if phase is None or (output / f"qdisc-{phase}-cutoff.json").exists():
        return None
    actors = proof.get("holder_pids")
    if not actors:
        return None
    result = {}
    for iface, role in (
        ("tserver", "server"),
        ("tclient", "client"),
        ("rserver", "router"),
        ("rclient", "router"),
    ):
        pid = actors[role]
        ns = f"/proc/{pid}/ns/net"
        inode = os.stat(ns).st_ino
        if inode != proof["namespaces"][role]["inode"]:
            raise RuntimeError(f"private {role} namespace changed before qdisc reading")
        pre = time.time()
        cmd = [
            "/usr/bin/sudo",
            "-n",
            "/usr/bin/nsenter",
            "--net=" + ns,
            "--",
            "/usr/sbin/tc",
            "-s",
            "qdisc",
            "show",
            "dev",
            iface,
        ]
        text = module.check_output(cmd, text=True, timeout=1.5)
        result[iface] = {"started": pre, "finished": time.time(), "stats": text}
    data = {"cutoff_unix": cutoff, "phase": phase, "interfaces": result}
    dump(output / f"qdisc-{phase}-cutoff.json", data)
    return data


def ids(path):
    return sorted(map(int, path.read_text().split()))


def members(scope):
    result = []
    for cg in [scope, *[p for p in scope.rglob("*") if p.is_dir()]]:
        for pid in ids(cg / "cgroup.procs"):
            try:
                proc = Path("/proc") / str(pid)
                cmd = (
                    (proc / "cmdline")
                    .read_bytes()
                    .decode(errors="replace")
                    .split("\0")[:-1]
                )
                threads = sorted([int(t.name) for t in (proc / "task").iterdir()])
                try:
                    executable = str((proc / "exe").resolve(strict=True))
                except PermissionError:
                    executable = None
                result.append(
                    {
                        "pid": pid,
                        "cgroup": str(cg),
                        "exe": executable,
                        "argv0": cmd[0] if cmd else None,
                        "threads": len(threads),
                        "affinities": sorted(
                            {
                                ",".join(map(str, sorted(os.sched_getaffinity(t))))
                                for t in threads
                            }
                        ),
                    }
                )
            except (FileNotFoundError, ProcessLookupError):
                continue
    return result


def service_snapshot(runtime):
    group = Path(runtime["scope"])
    fields = {}
    for f in [
        "cgroup.controllers",
        "cpu.max",
        "memory.max",
        "memory.swap.max",
        "memory.current",
        "memory.peak",
        "memory.events",
        "cpu.stat",
        "cgroup.events",
    ]:
        try:
            fields[f] = (group / f).read_text().strip()
        except FileNotFoundError:
            if f != "cpu.max":
                raise
            # A user systemd scope can delegate memory and CPU accounting while
            # omitting the per-scope CPU bandwidth controller. Affinity is read per thread.
            fields[f] = None
    return {
        "unix": time.time(),
        "unit": str(group),
        "server_pid": runtime["pid"],
        "resources": fields,
        "descendants": members(group),
    }


def read_timeline(path):
    return json.loads(path.read_text())["timeline"]


def occupancy(raw, args):
    plan = json.loads((raw / "plan.json").read_text())
    shards = [json.loads((raw / f"shard{i}.json").read_text()) for i in range(4)]
    observed = [{s["t"]: s["conns"] for s in d["timeline"]} for d in shards]
    ramp = plan["ramp_s"]
    hold = plan["hold_s"]
    full_set = range((ramp // 5 + 1) * 5, ramp + hold, 5)
    aligned = [{"t": t, "shards": [g.get(t) for g in observed]} for t in full_set]
    allowed = [g["users"] for g in plan["groups"]]
    strict = bool(aligned) and all(row["shards"] == allowed for row in aligned)
    return {
        "simultaneous_all_shards": strict,
        "expected": allowed,
        "hold_samples": aligned,
        "dials": [d["dials"] for d in shards],
        "raw_users": [d["users"] for d in shards],
        "scheduled_users": args.users,
        "full_score_duration_s": ramp + hold,
    }


def wire_evidence(samples, plan, shards):
    """Read scored deltas, excluding the endpoint teardown counters."""
    requested = {
        "server": ["tserver"],
        "router": ["rserver", "rclient"],
        "client": ["tclient"],
    }
    fields = {
        "rx_packets": 1,
        "rx_errors": 2,
        "rx_drops": 3,
        "tx_packets": 9,
        "tx_errors": 10,
        "tx_drops": 11,
    }
    ticks = ("user", "nice", "system", "idle", "iowait", "irq", "softirq", "steal")
    hz = plan["cpu_tick_hz"]

    def devices(point, role):
        return {
            iface.strip(): list(map(int, data.split()))
            for line in point["private_devices"][role]
            for iface, _, data in [line.partition(":")]
        }

    def group(start, stop):
        elapsed = stop["mono"] - start["mono"]
        deltas = {}
        for role, interfaces in requested.items():
            before, after = devices(start, role), devices(stop, role)
            for iface in interfaces:
                deltas[iface] = {
                    key: after[iface][index] - before[iface][index]
                    for key, index in fields.items()
                }
        cores = {}
        for n in sorted(
            set.union(*[set(x["cpus"]) for x in plan["groups"]]) | {9, 10, 14}
        ):
            k = str(n)
            row = {
                t: stop["host_core_counters"][k][t] - start["host_core_counters"][k][t]
                for t in ticks
            }
            tot = sum(row.values())
            cores[k] = {
                **row,
                "busy_pct": (
                    100 * (1 - (row["idle"] + row["iowait"]) / tot) if tot else None
                ),
            }
        client = []
        for i, g in enumerate(plan["groups"]):
            a, b = start["clients"][i], stop["clients"][i]
            client.append(
                {
                    "cpus": g["cpus"],
                    "mean_cores": (
                        (b["cpu_ticks"] - a["cpu_ticks"]) / hz / elapsed
                        if a.get("pid") and a.get("pid") == b.get("pid")
                        else None
                    ),
                }
            )
        drop_aggregate = sum(
            v[k]
            for v in deltas.values()
            for k in ("rx_drops", "tx_drops", "rx_errors", "tx_errors")
        )
        return {
            "start": start["scheduled_s"],
            "end": stop["scheduled_s"],
            "wall_s": elapsed,
            "network_counters": deltas,
            "wire_drop_or_error_events": drop_aggregate,
            "client_process_cpu": client,
            "host_core_ticks": cores,
            "service_cgroup_usage_s": (
                stop["server_cpu"]["usage_usec"] - start["server_cpu"]["usage_usec"]
            )
            / 1e6,
        }

    end = plan["ramp_s"] + plan["hold_s"]
    # Last complete sample where the scorer is still connected: late process-exit
    # races do not discard these CPU counters. The native run separately stores cutoff.
    by_t = {r["scheduled_s"]: r for r in samples}
    flags = []
    try:
        first = by_t[0]
        hold = by_t[plan["ramp_s"]]
        cutoff = by_t[end - 0.25]
        full = group(first, cutoff)
        steady = group(hold, cutoff)
        if full["wire_drop_or_error_events"]:
            flags.append("positive private scored RX/TX drop or error delta")
        return {
            "complete": True,
            "full": full,
            "hold": steady,
            "requests_live_only": sum(r["reqs"] for r in shards),
            "flags": flags,
        }
    except (KeyError, IndexError, TypeError, ZeroDivisionError) as exc:
        return {
            "complete": False,
            "error": str(exc),
            "flags": ["incomplete scored wire or CPU counters"],
        }
