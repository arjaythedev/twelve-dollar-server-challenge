"""Combine stock Go histograms with measured native TCP, device and CPU samples."""

import argparse
from datetime import datetime
import json
from pathlib import Path


def read(path):
    return json.loads(path.read_text())


def report(root):
    shape = read(root / "shape.json")
    score = read(root / "score.json")
    rows = [json.loads(line) for line in (root / "samples.jsonl").read_text().splitlines()]
    shards = [read(root / f"go{i}/score.json") for i in range(len(shape["groups"]))]
    zero = shape["start_ms"] / 1000
    duration = shape["ramp"] + shape["hold"]
    elapsed = lambda s: s["time_ns"] / 1e9 - zero
    total_est = lambda s: sum(a["CurrEstab"] for a in s["tcp"])
    all_rows = [s for s in rows if 0 <= elapsed(s) < duration]
    full = [s for s in all_rows if total_est(s) == shape["users"]]
    ticks = {}
    for shard in shards:
        for t in shard.get("timeline", []):
            ticks.setdefault(t["t"], []).append(t)
    aligned = {t: parts for t, parts in ticks.items() if len(parts) == len(shards)}
    result = {
        k: score[k]
        for k in (
            "users",
            "dials",
            "reqs",
            "req_per_s",
            "p50_ms",
            "p95_ms",
            "p99_ms",
            "err_pct",
            "fails",
            "pass",
            "connect_p95_ms",
            "connect_p99_ms",
        )
    }
    result.update(
        max_tcp_established=max(map(total_est, all_rows), default=None),
        max_go_open_5s=max((sum(t["conns"] for t in group) for group in aligned.values()), default=None),
        first_observed_all_open_s=round(elapsed(full[0]), 3) if full else None,
        confirmed_client_peak=(
            max((sum(a["CurrEstab"] for a in s.get("client_tcp", [])) for s in all_rows), default=0)
            if rows and "client_tcp" in rows[-1]
            else None
        ),
        peak_root_conntrack=max((s["host_conntrack"] for s in all_rows), default=None),
        summed_private_peak_conntrack=max((sum(s["netns_conntrack"]) for s in all_rows), default=None),
        source_binary_sha256=shape["server_sha256"],
    )
    core = shape["server_cpu"]
    hz = shape["physical_hz"]

    def stats(selection):
        selection = [s for s in selection if "usage_usec" in s["native"]]
        if len(selection) < 2:
            return None
        a, b = selection[0], selection[-1]
        dt = (b["mono_ns"] - a["mono_ns"]) / 1e9

        def cgroup(y, x):
            return (y["usage_usec"] - x["usage_usec"]) / (1e6 * dt)

        def cpu(i):
            x = a["cores"][f"cpu{i}"]
            y = b["cores"][f"cpu{i}"]
            return sum(y[k] - x[k] for k in ("user", "nice", "sys", "irq", "softirq")) / (hz * dt)

        native_before = a["cores"][f"cpu{core}"]
        native_after = b["cores"][f"cpu{core}"]
        include = [(t, parts) for t, parts in aligned.items() if elapsed(a) <= t - 5 and t <= elapsed(b)]
        return dict(
            begin_s=round(elapsed(a), 3),
            end_s=round(elapsed(b), 3),
            seconds=round(dt, 3),
            inclusive_native_cpu_cores=cpu(core),
            native_softirq_cpu_cores=(native_after["softirq"] - native_before["softirq"]) / (hz * dt),
            native_cgroup_cpu_cores=cgroup(b["native"], a["native"]),
            anchor_cgroups_cpu_cores=sum(cgroup(y, x) for y, x in zip(b["helpers"], a["helpers"])),
            go_cgroups_cpu_cores=[cgroup(y, x) for y, x in zip(b["clients"], a["clients"])],
            inclusive_client_cpu_cores=sum(
                cpu(i) for i in set(j for group in shape["groups"] for j in group)
            ),
            native_softnet_drops=native_after["drop"] - native_before["drop"],
            sampled_native_receive_drops=sum(
                y["rx_dropped"] - x["rx_dropped"] for y, x in zip(b["interfaces"], a["interfaces"])
            ),
            stock_rps_for_contained_complete_5s_windows=(
                (sum(p["reqs"] for t, parts in include for p in parts) / (5 * len(include)))
                if include
                else None
            ),
        )

    result["ramp"] = stats([s for s in all_rows if elapsed(s) < shape["ramp"]])
    result["fully_established_hold"] = stats([s for s in full if elapsed(s) >= shape["ramp"]])
    end = read(root / "end.json")
    result["native_cgroup_present_at_end_snapshot"] = "memory.peak" in end["native"]
    result["native_cgroup_peak_bytes"] = max(
        [end["native"].get("memory.peak", 0)] + [s["native"].get("memory.peak", 0) for s in rows]
    )
    sampled_peak = lambda cohort: max(
        (s["native"]["memory.peak"] for s in cohort if "memory.peak" in s["native"]), default=None
    )
    result["native_peak_observed_by_scorer_cutoff_bytes"] = sampled_peak(all_rows)
    after = [s for s in rows if elapsed(s) >= duration]
    after_peaks = [n for n in (sampled_peak(after), end["native"].get("memory.peak")) if n is not None]
    result["native_peak_observed_after_cutoff_bytes"] = max(after_peaks, default=None)
    result["native_oom_kills_max_observed"] = max(
        (
            s.get("memory.events.oom_kill", 0)
            for s in [end["native"], *(r["native"] for r in rows)]
            if "memory.events.oom_kill" in s
        ),
        default=None,
    )
    closure_path = root / "closure.jsonl"
    if closure_path.exists():
        close_rows = [json.loads(line) for line in closure_path.read_text().splitlines()]
        peak_keys = (
            ("native_cgroup_peak_bytes", close_rows),
            (
                "native_peak_observed_by_scorer_cutoff_bytes",
                [r for r in close_rows if r["relative_to_cutoff_s"] < 0],
            ),
            (
                "native_peak_observed_after_cutoff_bytes",
                [r for r in close_rows if r["relative_to_cutoff_s"] >= 0],
            ),
        )
        for key, cohort in peak_keys:
            peaks = [r["memory.peak"] for r in cohort if "memory.peak" in r]
            if result.get(key) is not None:
                peaks.append(result[key])
            result[key] = max(peaks, default=None)
        kill_events = [
            r["memory.events"]["oom_kill"] for r in close_rows if "oom_kill" in r.get("memory.events", {})
        ]
        if result["native_oom_kills_max_observed"] is not None:
            kill_events.append(result["native_oom_kills_max_observed"])
        result["native_oom_kills_max_observed"] = max(kill_events, default=None)
        result["native_first_observed_oom_kill_seconds_after_cutoff"] = next(
            (
                round(r["relative_to_cutoff_s"], 3)
                for r in close_rows
                if r.get("memory.events", {}).get("oom_kill", 0)
            ),
            None,
        )
    if (root / "post-close.json").exists():
        post = read(root / "post-close.json")
        result["post_close_verified"] = post.get("checks_pass")
        result["native_alive_before_post_close_health"] = post.get("native_alive_before_health")
        result["post_close_health_passes"] = sum(bool(r.get("ok")) for r in post.get("health", []))
        result["post_close_sqlite_integrity"] = post.get("poststop_db_verification", {}).get(
            "integrity_check"
        )
        result["post_close_sqlite_foreign_key_violations"] = post.get("poststop_db_verification", {}).get(
            "foreign_key_check"
        )
    journal = root / "service-journal.log"
    events = []
    if journal.exists():
        for line in journal.read_text().splitlines():
            if "oom" not in line.lower():
                continue
            try:
                stamp = datetime.fromisoformat(line.split()[0])
            except (ValueError, IndexError):
                continue
            if stamp.tzinfo is not None:
                events.append(
                    {
                        "utc": stamp.isoformat(),
                        "seconds_after_scorer_cutoff": round(stamp.timestamp() - zero - duration, 3),
                        "message": line,
                    }
                )
    result["scope_oom_journal_events"] = events
    result["sum_anchor_peak_bytes"] = sum(g.get("memory.peak", 0) for g in end["helpers"])
    if (root / "drain.json").exists():
        result["drain"] = read(root / "drain.json")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path)
    print(json.dumps(report(parser.parse_args().run_dir), indent=2))
