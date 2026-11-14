"""Correlate private TAP drop observations with already-recorded stock windows and delivered CPU."""

import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "router"))
from assess import read_json, rows, scheduler_accounting


def assess(path):
    shape = read_json(path / "shape.json")
    clock = read_json(path / "timing.json")
    zero = clock["stock_start_unix"]
    before_cutoff = [r for r in rows(path / "network-samples.jsonl") if r["unix"] < clock["cutoff_unix"]]
    if len(before_cutoff) < 2 or any("tap_interfaces" not in r for r in before_cutoff):
        raise ValueError("requires at least two recorded TAP-interface samples before the stock cutoff")
    app, host = rows(path / "samples.jsonl"), rows(path / "telemetry.jsonl")
    shards = [read_json(p)["timeline"] for p in sorted(path.glob("shard-*.json"))]
    if len(shards) != 4:
        raise ValueError("requires all four stock shard outputs")
    aligned = []
    maps = []
    for shard in shards:
        window = {}
        previous = 0
        for tick in shard:
            nearest = 5 * round(tick["t"] / 5)
            if abs(tick["t"] - nearest) <= 1:
                window[nearest] = (previous, tick)
            previous = tick["t"]
        maps.append(window)
    for t in sorted(set.intersection(*(set(m) for m in maps))):
        spans = [m[t] for m in maps]
        slab = [tick for _, tick in spans]
        aligned.append(
            {
                "ending_s": t,
                # Stock mload floors timestamps to an integer number of elapsed seconds.
                "earliest_shard_window_start_s": min(previous for previous, _ in spans),
                "latest_shard_window_end_upper_bound_s": max(tick["t"] for tick in slab) + 1,
                "scheduled_users": sum(q["users"] for q in slab),
                "generator_open_connections": sum(q["conns"] for q in slab),
                "failures_in_window": sum(q["fails"] for q in slab),
                "p99_ms_by_shard": [round(q["p99_ms"], 2) for q in slab],
            }
        )
    events = []
    service = shape["namespaces"][0]
    for a, b in zip(before_cutoff, before_cutoff[1:]):
        changes = []
        for namespace, iface in b["tap_interfaces"].items():
            for direction, counters in iface.items():
                diff = counters["dropped"] - a["tap_interfaces"][namespace][direction]["dropped"]
                if diff:
                    endpoint = (
                        "service"
                        if namespace == service
                        else f"generator-{shape['namespaces'][2:].index(namespace)+1}"
                    )
                    changes.append({"endpoint": endpoint, "direction": direction, "driver_drop_delta": diff})
        if not changes:
            continue
        before = a.get("collection_started_unix", a["unix"]) - zero
        observed = b["unix"] - zero
        previous_ticks = [q for q in aligned if q["latest_shard_window_end_upper_bound_s"] <= before][-3:]
        after_ticks = [q for q in aligned if q["earliest_shard_window_start_s"] >= observed][:3]
        host_cpu = scheduler_accounting(app, host, shape["app_cpu"], a["unix"], b["unix"])
        worker_sched = {}
        for worker in b["polling_relay"]["workers"]:
            tid = str(worker["tid"])
            x, y = a.get("relay_worker_scheduling", {}).get(tid), b.get("relay_worker_scheduling", {}).get(
                tid
            )
            if x and y:
                worker_sched[worker["cpu"]] = {
                    "runqueue_wait_ms": round((y["runqueue_wait_ns"] - x["runqueue_wait_ns"]) / 1e6, 3),
                    "involuntary_switches": y["nr_involuntary_switches"] - x["nr_involuntary_switches"],
                }
        events.append(
            {
                "previous_collected_interval_s": [round(before, 3), round(a["unix"] - zero, 3)],
                "drop_observation_interval_s": [
                    round(b.get("collection_started_unix", b["unix"]) - zero, 3),
                    round(observed, 3),
                ],
                "new_native_drops": changes,
                "preceding_stock_windows": previous_ticks,
                "following_stock_windows": after_ticks,
                "scored_cpu_during_uncertain_interval": host_cpu,
                "external_worker_scheduling_delta": worker_sched,
            }
        )
    return {
        "trial": path.name,
        "profile": shape["topology"],
        "app_cpu": shape["app_cpu"],
        "events": events,
        "meaning": "A driver drop occurred between the two interface observations. Stock p99 values here are original five-second per-shard window statistics. Exposure timing and task clock ownership are sampled independently.",
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "runs", metavar="RUN_DIRECTORY", type=Path, nargs="+",
        help="finished TAP run where this command writes external-loss-timing.json",
    )
    for path in parser.parse_args().runs:
        directory = path.resolve()
        result = assess(directory)
        (directory / "external-loss-timing.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps({"trial": result["trial"], "observed_intervals": len(result["events"])}))
