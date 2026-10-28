#!/usr/bin/env python3
"""Combine k6 summaries and sampled cgroup data without changing raw results."""
import argparse
import csv
from datetime import datetime, timedelta, timezone
import json
import math
from pathlib import Path
import statistics

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("directories", nargs="+", type=Path)
parser.add_argument("--output", type=Path)
args = parser.parse_args()


def seconds(text):
    return float(text[:-1]) * {"s": 1, "m": 60}[text[-1]]


def maximum(rows, name):
    return max((float(row[name]) for row in rows if row.get(name)), default=0)


def diagnostics(output, start, end, metadata):
    path = output / "counters.csv"
    if not path.is_file():
        return None
    if metadata.get("counter_timestamp_timezone") != "UTC":
        return {"note": "Counter timestamps have no verified timezone; raw data preserved."}
    values = {}
    with path.open(newline="") as stream:
        for row in csv.DictReader(stream):
            when = datetime.strptime(row["Timestamp"], "%m/%d/%Y %H:%M:%S").replace(tzinfo=timezone.utc)
            if not start <= when <= end or not row["Mean/Increment"]:
                continue
            value = float(row["Mean/Increment"])
            if math.isfinite(value):
                values.setdefault(row["Counter Name"], []).append(value)
    gauges = {}
    for name in ("feed.db.write_queued", "feed.db.write_active", "feed.runtime.worker_threads", "feed.runtime.pending_work"):
        rows = [value for key, group in values.items() if key.startswith(name + " (") for value in group]
        if rows:
            gauges[name] = {"mean": statistics.fmean(rows), "max": max(rows), "nonzero_samples": sum(value != 0 for value in rows)}
    allocation = next((group for name, group in values.items() if name.startswith("dotnet.gc.heap.total_allocated (")), [])
    return {
        "method": "Central-hold samples every 2 seconds. Histogram rows are interval quantiles, not whole-run percentiles. Runtime UpDownCounter rates are not treated as absolute queue/thread counts.",
        "gc_allocated_mib_per_s": sum(allocation) / (len(allocation) * 2 * 1024**2) if allocation else None,
        "gauges": gauges,
        "interval_histograms": {name: {"median": statistics.median(group), "max": max(group), "samples": len(group)}
                                for name, group in sorted(values.items()) if name.startswith(("feed.db.duration (", "feed.db.commit (", "feed.db.write_wait ("))},
    }


results = []
for directory in args.directories:
    for summary_path in sorted(directory.glob("*/measured-summary.json")):
        output = summary_path.parent
        metadata = json.loads((output / "metadata.json").read_text())
        summary = json.loads(summary_path.read_text())
        metrics = summary["metrics"]
        with (output / "server.csv").open(newline="") as stream:
            samples = list(csv.DictReader(stream))
        finished = datetime.fromisoformat(summary["local"]["finished_utc"])
        started = finished - timedelta(milliseconds=summary["state"]["testRunDurationMs"])
        hold_start = started + timedelta(seconds=seconds(metadata["ramp"]) + 2)
        hold_end = started + timedelta(seconds=seconds(metadata["ramp"]) + seconds(metadata["hold"]))
        steady = [row for row in samples if hold_start <= datetime.fromisoformat(row["utc"]) <= hold_end]
        if not steady:
            raise ValueError("No steady-hold samples: " + str(output))
        duration = metrics["http_req_duration"]["values"]
        failed = metrics["http_req_failed"]["values"]
        result = {
            "vus": metadata["vus"], "hold": metadata["hold"], "ramp": metadata["ramp"],
            "pass": metadata["measured_run"]["exit_code"] == 0,
            "requests": metrics["http_reqs"]["values"]["count"],
            "overall_rps_including_ramps": metrics["http_reqs"]["values"]["rate"],
            "p95_ms": duration["p(95)"], "p99_ms": duration["p(99)"], "max_ms": duration["max"],
            "errors": failed["passes"], "error_pct": failed["rate"] * 100,
            "steady_cpu_pct_one_core": statistics.fmean(float(row["cpu_pct_one_core"]) for row in steady),
            "steady_cpu_peak_2s_pct": maximum(steady, "cpu_pct_one_core"),
            "cgroup_peak_mib": maximum(samples, "memory_peak_bytes") / 1024**2,
            "process_peak_rss_mib_observed": maximum(samples, "process_rss_bytes") / 1024**2,
            "wal_peak_mib_observed": maximum(samples, "wal_bytes") / 1024**2,
            "swap_mib_observed": maximum(samples, "swap_bytes") / 1024**2,
            "oom_kills_observed": maximum(samples, "oom_kill"),
            "client_steady_cpu_pct_one_core": statistics.fmean(float(row["client_cpu_pct_one_core"]) for row in steady
                                                               if row.get("client_cpu_pct_one_core")),
            "client_peak_rss_mib_observed": maximum(samples, "client_peak_rss_bytes") / 1024**2,
            "client_swap_mib_observed": maximum(samples, "client_swap_bytes") / 1024**2,
            "endpoints": {}, "raw_directory": str(output),
        }
        counter_summary = diagnostics(output, hold_start, hold_end, metadata)
        if counter_summary is not None:
            result["diagnostics"] = counter_summary
        for endpoint in ("GET /feed", "GET /posts/:id", "POST /posts/:id/like", "POST /posts"):
            latency = metrics[f"http_req_duration{{name:{endpoint}}}"]["values"]
            result["endpoints"][endpoint] = {
                "count": metrics[f"http_reqs{{name:{endpoint}}}"]["values"]["count"],
                "errors": metrics[f"http_req_failed{{name:{endpoint}}}"]["values"]["passes"],
                "p95_ms": latency["p(95)"], "p99_ms": latency["p(99)"], "max_ms": latency["max"],
                "error_pct": metrics[f"http_req_failed{{name:{endpoint}}}"]["values"]["rate"] * 100}
        results.append(result)

data = {"method": "Unchanged challenge workload. Server cgroup: 1 CPU quota, 2 GiB, no swap. k6 outside that cgroup on same WSL VM.",
        "interpretation": "Local diagnostic only. RPS includes ramp/drain; CPU averages use central hold samples. Memory includes cgroup page cache, excludes OS outside cgroup. Observed peaks sampled every 2 seconds.",
        "runs": results}
text = json.dumps(data, indent=2)
if args.output:
    args.output.write_text(text + "\n")
print(text)
