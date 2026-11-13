"""Derive strict qualification from timestamped samples and the four original stock JSONs."""

import json
from pathlib import Path
import re


def read(path):
    return json.loads(path.read_text())


def qdisc_drop(text):
    matches = re.findall(r"\bdropped (\d+)", text)
    return sum(map(int, matches)) if matches else None


def scheduled_dials_match(occupancy, merged, scheduled_users):
    expected = occupancy["expected"]
    return (
        len(expected) == 4
        and sum(expected) == scheduled_users
        and occupancy["raw_users"] == expected
        and occupancy["dials"] == expected
        and merged["dials"] == scheduled_users
    )


def classify_lifetime(samples, fail, pid, cutoff):
    points = sorted(samples, key=lambda x: x["wall"])
    live = [
        (
            x["wall"],
            x["cgroup_live_sample"],
            x.get("server_pid", {}).get("pid") == pid,
            x.get("server_mem", {}).get("events", {}).get("oom_kill"),
        )
        for x in points
    ]
    certain_pre_oom = any(
        t <= cutoff and fresh and isinstance(oom, int) and oom > 0
        for t, fresh, _, oom in live
    )
    caught_live_after = next(
        (
            (t, oom)
            for t, fresh, alive, oom in live
            if t >= cutoff and fresh and alive and isinstance(oom, int)
        ),
        None,
    )
    last_live = max((t for t, fresh, alive, _ in live if fresh and alive), default=None)
    first_gone = (
        next((t for t, fresh, alive, _ in live if not alive and t >= live[0][0]), None)
        if live
        else None
    )
    pre_failure = first_gone is not None and first_gone <= cutoff
    if fail is not None and fail["elapsed_seconds"] is not None:
        pre_failure |= bool(fail["recorded_unix"] <= cutoff)
    if pre_failure or certain_pre_oom:
        app = "scored_oom" if certain_pre_oom else "scored_exit"
    elif caught_live_after is None:
        app = "cutoff_unresolved"
    elif caught_live_after[1] > 0:
        # The first fresh counter after cutoff exceeds zero. A previous positive
        # causal sample is needed to classify the kill as scored or definitively late.
        prior = [
            p
            for p in live
            if p[0] >= cutoff and p[0] <= caught_live_after[0] and p[1] and p[3] == 0
        ]
        app = "post_cutoff_oom" if prior else "cutoff_unresolved"
    elif fail or any(
        t >= caught_live_after[0] and fresh and isinstance(oom, int) and oom > 0
        for t, fresh, _, oom in live
    ):
        app = "post_cutoff_failure"
    else:
        app = "alive_at_cutoff"
    return {
        "state": app,
        "last_observed_service_alive_unix": last_live,
        "first_observed_missing_unix": first_gone,
        "first_fresh_alive_at_or_after_cutoff": (
            list(caught_live_after) if caught_live_after else None
        ),
        "archived_exit": fail,
    }


def qdisc_window(out, original, proof, cutoff):
    after = {
        **original["private_router_qdiscs_after"],
        "tserver": original["private_server_qdisc_after"],
        "tclient": original["private_client_qdisc_after"],
    }
    evidence = {}
    reads = {}
    for phase in ("before", "after"):
        source = out / f"qdisc-{phase}-cutoff.json"
        if source.is_file():
            body = read(source)
            if abs(body.get("cutoff_unix", 0) - cutoff) > 0.02:
                raise ValueError(f"{phase} qdisc read belongs to another time interval")
            reads[phase] = body.get("interfaces", {})
    for port in ("tserver", "tclient", "rserver", "rclient"):
        before = qdisc_drop(proof.get("qdisc_before", {}).get(port, ""))
        final = qdisc_drop(after.get(port, ""))
        measured = []
        for phase, records in reads.items():
            x = records.get(port, {})
            value = qdisc_drop(x.get("stats", ""))
            if value is not None:
                measured.append([x["started"], x["finished"], value])
        # Source plan captures private qdisc stats at construction, before stock
        # sockets. An original total of zero retrospectively proves zero throughout.
        if before is None:
            state = "clean" if final == 0 else "cutoff_unresolved"
        elif final is None or final < before:
            state = "cutoff_unresolved"
        elif final == before:
            state = "clean"
        elif any(end <= cutoff and value > before for _, end, value in measured):
            state = "scored_drop"
        elif any(start >= cutoff and value == before for start, _, value in measured):
            state = "post_cutoff_drop"
        else:
            state = "cutoff_unresolved"
        evidence[port] = {
            "before": before,
            "final_after_stock_merge": final,
            "timed_readings": measured,
            "state": state,
        }
    return evidence


def assess(out, plan, original, merged, occ, wire, rc):
    raw = out / "raw"
    runtime = read(raw / "runtime.json")
    start = runtime["start_unix_ms"] / 1000
    cutoff = start + plan["ramp_s"] + plan["hold_s"]
    expected = occ["expected"]
    samples = [json.loads(x) for x in (raw / "samples.jsonl").read_text().splitlines()]
    proof = read(out / "placements.json")
    actors = [
        json.loads(x)
        for x in (out / "charged-descendants.jsonl").read_text().splitlines()
    ]
    fail = None
    if (raw / "app_fail.json").is_file():
        fail = read(raw / "app_fail.json")
        elapsed = fail.get("elapsed_seconds")
        fail = {
            **fail,
            "elapsed_seconds": elapsed,
            "recorded_unix": (
                start + elapsed if isinstance(elapsed, (float, int)) else None
            ),
        }
    life = classify_lifetime(samples, fail, runtime["pid"], cutoff)
    flags = list(wire.get("flags", []))
    shape = read(out / "shape.json")
    for field in (
        "server_sha256",
        "seed_sha256",
        "tokens_sha256",
        "hold_s",
        "ramp_s",
        "users",
    ):
        if shape.get(field) != plan.get(field):
            flags.append("archived plan and shape differ for " + field)
    if (
        plan.get("generator_sha256") != shape.get("mload_sha256")
        or shape.get("mload_sha256")
        != "48df1cd6242f55ee5be98238a54fbf7861fcc83c6e3cc91d334b7903d4f208e6"
    ):
        flags.append("stock scorer fingerprint differs")
    if (
        merged.get("users") != plan["users"]
        or abs(merged.get("duration_s", 0) - plan["ramp_s"] - plan["hold_s"]) > 2
    ):
        flags.append("stock users or full score duration differs")
    if life["state"] == "cutoff_unresolved":
        flags.append("service liveness/OOM cutoff unresolved")
    qdiscs = qdisc_window(out, original, proof, cutoff)
    for iface, arr in qdiscs.items():
        if arr["state"] in ("scored_drop", "cutoff_unresolved"):
            flags.append(f"{iface} qdisc {arr['state']}")
    if (
        proof.get("lanes") is None
        or len(proof["lanes"]) != 4
        or sorted(x.get("lane") for x in proof["lanes"]) != [0, 1, 2, 3]
    ):
        flags.append("four independent CPU placement proof rows missing")
    else:
        for i, row in enumerate(proof["lanes"]):
            pin = plan["groups"][i]["cpus"][0]
            if (
                "incoming_cpu=10:24," not in row["server"]
                or "mtu=1500" not in row["server"]
                or "mtu=1500" not in row["client"]
                or row["target"] != f"10.240.{i+1}.1"
                or row["source"] != f"10.243.{i+1}.1"
                or f"process_cpu={pin} " not in row["client"]
            ):
                flags.append(f"invalid two-sided placement for shard {i}")
    bases = {
        r: proof.get("namespaces", {}).get(r, {})
        for r in ("server", "client", "router")
    }
    if len({x.get("inode") for x in bases.values()}) != 3 or any(
        "ipv4/tcp_ehash_entries" not in x for x in bases.values()
    ):
        flags.append("private namespace identity or signed kernel table unavailable")
    if not scheduled_dials_match(occ, merged, plan["users"]):
        flags.append("four stock shard dial counts differ from scheduled users")
    trace = {"per_shard": True, "issues": []}
    for i, target in enumerate(expected):
        # dials are successful TCP opens, not a distinct-VU counter. Reaching
        # each shard's entire assigned cohort in one sample establishes the
        # distinct presence independently of the successful-open counter.
        if not any(
            len(row["shards"]) > i and row["shards"][i] == target
            for row in occ["hold_samples"]
        ):
            trace["issues"].append(
                f"shard {i} never sampled all assigned VUs connected"
            )
    trace["per_shard"] = not trace["issues"]
    charge = [a for a in actors if start - 5 <= a["unix"] <= cutoff]

    def charged(a):
        members = a.get("descendants", [])
        return (
            a["resources"].get("memory.max") == str(1750 * 1024**2)
            and a["resources"].get("memory.swap.max") == "0"
            and a["server_pid"] == runtime["pid"]
            and any(x["pid"] == runtime["pid"] for x in members)
            and all(x.get("affinities") == ["10"] for x in members)
        )

    if not charge or not all(charged(a) for a in charge):
        flags.append("service scope/state or descendant affinity")
    else:
        t = sorted(a["unix"] for a in charge)
        if (
            t[0] > start + 6
            or t[-1] < cutoff - 6
            or any(b - a > 8 for a, b in zip(t, t[1:]))
        ):
            flags.append("periodic charged-service coverage is incomplete")
    if set(original["conntrack_after"]) != {"server", "client", "router"} or any(
        int(v) for v in original["conntrack_after"].values()
    ):
        flags.append("conntrack reading missing or active")
    if rc:
        flags.append(f"historical runner exit {rc}")
    # Prove the terminal interval as well as the independently measured 250 ms-early
    # scored window. A loss first visible in the straddling baseline→cutoff pair
    # has no unique owner in time and cannot qualify the run.
    cutoff_sample = next(
        (s for s in samples if s["scheduled_s"] == plan["ramp_s"] + plan["hold_s"]),
        None,
    )
    link_ends = {}
    for role, interfaces in (
        ("server", ["tserver"]),
        ("client", ["tclient"]),
        ("router", ["rserver", "rclient"]),
    ):

        def drops(sample):
            result = {}
            for row in sample.get("private_devices", {}).get(role, []):
                iface, _, cols = row.partition(":")
                vals = cols.split()
                if iface.strip() in interfaces and len(vals) >= 12:
                    result[iface.strip()] = [int(vals[i]) for i in (2, 3, 10, 11)]
            return result

        start_read = next((s for s in samples if s["scheduled_s"] == 0), {})
        a = drops(start_read)
        b = drops(cutoff_sample or {})
        for port in interfaces:
            if (
                cutoff_sample is None
                or cutoff_sample["wall"] < cutoff
                or port not in a
                or port not in b
            ):
                state = "cutoff_unresolved"
            elif a[port] == b[port]:
                state = "clean"
            elif (
                wire.get("complete")
                and sum(
                    wire["full"]["network_counters"][port][key]
                    for key in ("rx_drops", "tx_drops", "rx_errors", "tx_errors")
                )
                > 0
            ):
                state = "scored_drop"
            else:
                state = "cutoff_unresolved"
            link_ends[port] = {
                "state": state,
                "at_start": a.get(port),
                "through_cutoff_read": b.get(port),
            }
            if state == "cutoff_unresolved":
                flags.append(f"{port} private interface counters at cutoff unresolved")
    app_scored = life["state"] in ("scored_oom", "scored_exit")
    return {
        "cutoff_unix": cutoff,
        "lifetime": life,
        "qdiscs": qdiscs,
        "private_link_cutoff": link_ends,
        "service_scope_samples": len(actors),
        "flags": flags,
        "dial_presence": trace,
        "app_scored": app_scored,
        "scored_oom": life["state"] == "scored_oom",
        "scope_samples_through_cutoff": len(charge),
        "stock_pass": bool(merged["pass"]),
        "qualified": bool(
            merged["pass"]
            and not flags
            and not app_scored
            and trace["per_shard"]
            and occ["simultaneous_all_shards"]
        ),
    }
