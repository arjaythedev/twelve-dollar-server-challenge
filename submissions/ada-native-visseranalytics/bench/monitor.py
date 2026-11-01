#!/usr/bin/env python3
"""Read-only Linux process/host/SQLite telemetry. Output one JSON line/sample."""
import argparse
import datetime
import json
import os
from pathlib import Path
import signal
import time


def process(pid):
    # comm may contain spaces or parentheses: split after its final ')'.
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return {
        "pid": pid,
        "start_ticks": int(fields[19]),
        "cpu_ticks": int(fields[11]) + int(fields[12]),
        "rss_kib": int(fields[21]) * os.sysconf("SC_PAGE_SIZE") // 1024,
    }


def tree(root, expected_start):
    result = []
    pending = [root]
    seen = set()
    while pending:
        pid = pending.pop()
        if pid in seen:
            continue
        seen.add(pid)
        try:
            item = process(pid)
            if pid == root and item["start_ticks"] != expected_start:
                return [], "root PID was reused"
            result.append(item)
            children = Path(f"/proc/{pid}/task/{pid}/children").read_text()
            pending.extend(int(child) for child in children.split())
        except (OSError, ValueError, IndexError) as error:
            if pid == root:
                return [], str(error)
            # A worker may exit between the process and children reads.
    return result, None


def size(path):
    try:
        return path.stat().st_size
    except FileNotFoundError:
        return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-pid", type=int, required=True)
    parser.add_argument("--nginx-pid", type=int)
    parser.add_argument("--pid-file", type=Path)
    parser.add_argument("--db", type=Path, required=True)
    parser.add_argument("--interval", type=float, default=5.0)
    parser.add_argument("--samples", type=int, default=0, help="0 means until signalled")
    args = parser.parse_args()
    if args.interval <= 0 or args.samples < 0 or args.app_pid <= 0 or (args.nginx_pid is not None and args.nginx_pid <= 0):
        parser.error("positive PIDs/interval and nonnegative samples required")
    roots = {"app": args.app_pid}
    if args.nginx_pid is not None:
        roots["nginx"] = args.nginx_pid
    if args.pid_file:
        args.pid_file.write_text(str(os.getpid()))
    starts = {role: process(pid)["start_ticks"] for role, pid in roots.items()}
    running = True

    def stop(_signum, _frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    previous = {}
    previous_time = None
    ticks_per_second = os.sysconf("SC_CLK_TCK")
    sample = 0
    while running and (args.samples == 0 or sample < args.samples):
        now = time.monotonic()
        elapsed = None if previous_time is None else now - previous_time
        record = {
            "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "interval_s": elapsed,
            "processes": [],
            "roles": {},
        }
        current = {}
        for role, pid in roots.items():
            members, error = tree(pid, starts[role])
            percentages = []
            for item in members:
                key = (item["pid"], item["start_ticks"])
                old = previous.get(key)
                percentage = None if old is None or elapsed is None else (
                    100.0 * (item["cpu_ticks"] - old) / ticks_per_second / elapsed
                )
                current[key] = item["cpu_ticks"]
                record["processes"].append(dict(item, role=role, cpu_pct=percentage))
                percentages.append(percentage)
            record["roles"][role] = {
                "rss_kib_sum": sum(item["rss_kib"] for item in members),
                "cpu_pct_sum": sum(percentages) if percentages and None not in percentages else None,
                "error": error,
            }
        wanted = {"MemTotal", "MemAvailable", "SwapTotal", "SwapFree"}
        record["memory_kib"] = {
            parts[0].rstrip(":"): int(parts[1])
            for line in Path("/proc/meminfo").read_text().splitlines()
            if (parts := line.split())[0].rstrip(":") in wanted
        }
        filesystem = os.statvfs(args.db.parent)
        record["disk_bytes"] = {
            "total": filesystem.f_blocks * filesystem.f_frsize,
            "available": filesystem.f_bavail * filesystem.f_frsize,
        }
        record["sqlite_bytes"] = {
            "db": size(args.db),
            "wal": size(Path(str(args.db) + "-wal")),
            "shm": size(Path(str(args.db) + "-shm")),
        }
        record["cgroup_v2"] = {}
        for name in ("memory.current", "memory.peak", "memory.max",
                     "memory.swap.max", "memory.events", "cpu.max", "cpu.stat"):
            try:
                record["cgroup_v2"][name] = Path("/sys/fs/cgroup", name).read_text().strip()
            except FileNotFoundError:
                pass
        print(json.dumps(record, separators=(",", ":")), flush=True)
        previous, previous_time = current, now
        sample += 1
        if running and (args.samples == 0 or sample < args.samples):
            time.sleep(args.interval)


if __name__ == "__main__":
    main()
