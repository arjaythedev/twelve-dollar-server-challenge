"""Sample a local trial's application cgroup after the stock score cutoff."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time


def snapshot(group):
    sample = {
        name: int((group / name).read_text())
        for name in ("memory.current", "memory.peak", "memory.swap.current")
    }
    for name, keys in (
        ("memory.stat", ("sock", "anon", "file", "kernel", "slab")),
        ("memory.events", ("high", "max", "oom", "oom_kill")),
    ):
        values = dict(line.split() for line in (group / name).read_text().splitlines())
        sample[name] = {key: int(values[key]) for key in keys}
    return sample


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trial", type=Path, help="active four-generator trial directory")
    parser.add_argument("--output", type=Path, help="destination (default: trial/teardown-memory.json)")
    args = parser.parse_args()
    shape = json.loads((args.trial / "shape.json").read_text())
    timing = json.loads((args.trial / "timing.json").read_text())
    unit = shape["unit"]
    if not re.fullmatch(r"twelve-revisit-tr[0-9a-f]+\.service", unit):
        raise ValueError(f"unrecognized trial unit {unit!r}")
    cpu = shape["manager_cpu"]
    os.sched_setaffinity(0, {cpu})
    cutoff = timing["cutoff_unix"]
    time.sleep(max(0.0, cutoff - time.time()))
    group_name = subprocess.check_output(
        ["systemctl", "show", unit, "--property=ControlGroup", "--value"], text=True
    ).strip()
    group = Path("/sys/fs/cgroup") / group_name.lstrip("/")
    if group.name != unit or not group.is_dir():
        raise RuntimeError(f"the trial's cgroup has already exited: {unit}")
    observations = []
    while time.time() < cutoff + 185:
        try:
            data = snapshot(group)
        except FileNotFoundError:
            break
        observations.append({"after_cutoff_s": round(time.time() - cutoff, 3), **data})
        time.sleep(0.25)
    if not observations:
        raise RuntimeError("no post-cutoff cgroup samples")
    report = {
        "unit": unit,
        "cpu": cpu,
        "cutoff_unix": cutoff,
        "sample_period_s": 0.25,
        "first": observations[0],
        "last": observations[-1],
        "max_cgroup_peak_bytes": max(row["memory.peak"] for row in observations),
        "observations": observations,
    }
    destination = args.output or args.trial / "teardown-memory.json"
    with tempfile.NamedTemporaryFile(mode="w", dir=destination.parent) as output:
        json.dump(report, output, separators=(",", ":"))
        output.write("\n")
        output.flush()
        os.link(output.name, destination)


if __name__ == "__main__":
    main()
