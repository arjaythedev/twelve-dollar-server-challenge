"""Compare the four stock clients' CPU and latency around the end of admission."""

import argparse
import json
from pathlib import Path
from statistics import median
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "router"))
from assess import cpu_windows, read_json, rows


def core_numbers(affinity):
    cores = []
    for item in affinity.split(","):
        first, _, last = item.partition("-")
        cores.extend(range(int(first), int(last or first) + 1))
    return cores


def measure(directory):
    shape = read_json(directory / "shape.json")
    timing = read_json(directory / "timing.json")
    group_masks = shape["client_cpus"]
    if len(group_masks) != 4:
        raise ValueError(f"{directory.name}: the comparison requires four stock clients")
    hold = timing["hold_starts_unix"]
    begin, end = hold - 15, hold + 10
    telemetry = rows(directory / "telemetry.jsonl")
    cpu = [cpu_windows(telemetry, core_numbers(mask), begin, end) for mask in group_masks]
    shards = [read_json(directory / f"shard-{i}.json") for i in range(len(cpu))]
    windows = [{tick["t"]: tick for tick in shard["timeline"]} for shard in shards]
    ramp = shape["ramp_s"]
    common = sorted(t for t in set.intersection(*(set(w) for w in windows))
                    if ramp - 15 <= t - 5 and t <= ramp + 10)
    if not common or any(not group for group in cpu):
        raise ValueError(f"{directory.name}: missing client timeline or CPU samples around the ramp")
    peak = max(common, key=lambda t: median(window[t]["p99_ms"] for window in windows))
    return {
        "trial": directory.name,
        "ramp_s": ramp,
        "client_cpu_groups": group_masks,
        "group_mean_cpu_busy_pct": [group["mean_busy_pct"] for group in cpu],
        "group_p95_sampled_cpu_busy_pct": [group["p95_interval_busy_pct"] for group in cpu],
        "heaviest_common_window_end_s": peak,
        "shard_p99_ms_in_that_window": [round(window[peak]["p99_ms"], 2) for window in windows],
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trials", type=Path, nargs="+")
    args = parser.parse_args()
    print(json.dumps([measure(trial) for trial in args.trials], indent=2, allow_nan=False))
