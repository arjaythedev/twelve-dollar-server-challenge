"""Record free space and SQLite file allocation during a local stock trial."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import time


def sample(trial):
    filesystem = os.statvfs(trial.parent)
    row = {"unix": time.time(), "available_bytes": filesystem.f_bavail * filesystem.f_frsize}
    for name in ("feed.db", "feed.db-wal"):
        try:
            stat = (trial / name).stat()
        except FileNotFoundError:
            continue
        row[name] = {
            "size": stat.st_size, "allocated": stat.st_blocks * 512,
            "device": f"{os.major(stat.st_dev)}:{os.minor(stat.st_dev)}",
        }
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trial", required=True, type=Path)
    parser.add_argument("--manager-cpu", required=True, type=int)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("supply the full stock driver command")
    trial = args.trial.resolve()
    if trial.exists() or not trial.parent.is_dir():
        parser.error("use a new trial directory inside the selected volume")
    if args.manager_cpu not in os.sched_getaffinity(0):
        parser.error("the sampling CPU is outside this process's affinity")
    identity = subprocess.check_output([
        "findmnt", "--json", "--target", str(trial.parent),
        "--output", "SOURCE,FSTYPE,MAJ:MIN,OPTIONS",
    ], text=True)
    mount, = json.loads(identity)["filesystems"]
    samples = [sample(trial)]
    process = subprocess.Popen(command)
    os.sched_setaffinity(0, {args.manager_cpu})
    while True:
        try:
            rc = process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            samples.append(sample(trial))
            continue
        break
    samples.append(sample(trial))
    if trial.is_dir():
        report = {
            "filesystem": mount, "sample_cpu": args.manager_cpu,
            "sample_period_s": 5, "samples": samples,
        }
        destination = trial / "database-volume.json"
        temporary = destination.with_suffix(".partial")
        temporary.write_text(json.dumps(report, separators=(",", ":")) + "\n")
        temporary.replace(destination)
    raise SystemExit(rc)


if __name__ == "__main__":
    main()
