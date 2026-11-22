#!/usr/bin/env python3
"""Prints one run's sampler time series (build/linuxbench/samples/<time>.jsonl) in buckets.

    python3 bench/samples.py [build/linuxbench/samples/20261015-220000.jsonl] [--bucket 5]   # default: newest

Per bucket: seconds since the hold began (negative = ramp), connections the kernel dropped from the
full accept queue, CPU 0 busy (softirq part), and the server's open, orphaned and TIME_WAIT sockets.
Stdlib only and Python 3.9 compatible.
"""

import argparse
import json
from pathlib import Path

SAMPLES = Path(__file__).resolve().parent.parent / "build" / "linuxbench" / "samples"


def buckets(rows: list, seconds: float) -> list:
    hold_from = rows[0]["hold_from"]
    out, current = [], [rows[0]]
    for row in rows[1:]:
        current.append(row)
        if row["t"] - current[0]["t"] >= seconds:
            out.append(summarize(current, hold_from))
            current = [row]
    if len(current) > 1:
        out.append(summarize(current, hold_from))
    return out


def summarize(rows: list, hold_from: float) -> dict:
    first, last = rows[0], rows[-1]
    delta = [b - a for a, b in zip(first["cpu0"], last["cpu0"])]  # user nice system idle iowait irq softirq steal
    total = max(1, sum(delta))
    return {
        "at_s": round(first["t"] - hold_from),
        "dropped": last["overflows"] - first["overflows"],
        "cpu0": (total - delta[3] - delta[4]) / total,
        "softirq": delta[6] / total,
        "sockets": max(row["sockets"] for row in rows),
        "orphans": max(row["orphans"] for row in rows),
        "time_wait": max(row["time_wait"] for row in rows),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("file", nargs="?", help="default: the newest run's samples")
    parser.add_argument("--bucket", type=float, default=5.0, help="seconds per row")
    args = parser.parse_args()
    if args.file is None:
        newest = sorted(SAMPLES.glob("*.jsonl"))
        if not newest:
            raise SystemExit("no samples yet: run `just bench linux <users>` first")
        args.file = str(newest[-1])
        print(args.file)
    with open(args.file) as f:
        rows = [json.loads(line) for line in f if line.strip()]
    print(f"{'at s':>6} {'dropped':>8} {'cpu0':>6} {'softirq':>8} {'sockets':>8} {'orphans':>8} {'time_wait':>10}")
    for row in buckets(rows, args.bucket):
        print(f"{row['at_s']:>6} {row['dropped']:>8} {row['cpu0']:>6.0%} {row['softirq']:>8.0%} "
              f"{row['sockets']:>8} {row['orphans']:>8} {row['time_wait']:>10}")


if __name__ == "__main__":
    main()
