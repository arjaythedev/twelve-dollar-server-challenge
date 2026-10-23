#!/usr/bin/env python3
"""Sample one explicitly supplied cgroup/PID; no application requests or writes."""
import argparse
import csv
from datetime import datetime, timezone
import os
from pathlib import Path
import signal
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cgroup", required=True)
parser.add_argument("--pid", type=int, required=True)
parser.add_argument("--database", required=True)
parser.add_argument("--output", required=True)
parser.add_argument("--client-pid-file", required=True)
args = parser.parse_args()
cgroup = Path("/sys/fs/cgroup") / args.cgroup.lstrip("/")
running = True


def stop(*_):
    global running
    running = False


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)


def fields(name):
    return {line.split()[0]: int(line.split()[1]) for line in (cgroup / name).read_text().splitlines()}


def number(name):
    path = cgroup / name
    return int(path.read_text()) if path.exists() else 0


with open(args.output, "w", newline="", buffering=1) as output:
    names = ["utc", "elapsed_s", "cpu_pct_one_core", "memory_current_bytes", "memory_peak_bytes",
             "process_rss_bytes", "swap_bytes", "wal_bytes", "cpu_usage_usec", "cpu_throttled_usec",
             "cpu_periods", "cpu_throttled_periods", "oom", "oom_kill"]
    names += ["client_pid", "client_cpu_pct_one_core", "client_rss_bytes", "client_peak_rss_bytes", "client_swap_bytes"]
    writer = csv.DictWriter(output, fieldnames=names)
    writer.writeheader()
    started = previous_time = time.monotonic()
    previous_cpu = fields("cpu.stat")["usage_usec"]
    previous_client_pid = previous_client_cpu = 0
    while running and cgroup.exists() and Path(f"/proc/{args.pid}").exists():
        now = time.monotonic()
        cpu = fields("cpu.stat")
        events = fields("memory.events")
        status = Path(f"/proc/{args.pid}/status").read_text().splitlines()
        rss = next((int(line.split()[1]) * 1024 for line in status if line.startswith("VmRSS:")), 0)
        wal = Path(args.database + "-wal")
        row = {
            "utc": datetime.now(timezone.utc).isoformat(), "elapsed_s": round(now - started, 3),
            "cpu_pct_one_core": round((cpu["usage_usec"] - previous_cpu) / max(now - previous_time, 0.001) / 10000, 2),
            "memory_current_bytes": number("memory.current"), "memory_peak_bytes": number("memory.peak"),
            "process_rss_bytes": rss, "swap_bytes": number("memory.swap.current"),
            "wal_bytes": wal.stat().st_size if wal.exists() else 0,
            "cpu_usage_usec": cpu["usage_usec"], "cpu_throttled_usec": cpu.get("throttled_usec", 0),
            "cpu_periods": cpu.get("nr_periods", 0), "cpu_throttled_periods": cpu.get("nr_throttled", 0),
            "oom": events.get("oom", 0), "oom_kill": events.get("oom_kill", 0),
        }
        client_pid_file = Path(args.client_pid_file)
        client_pid = int(client_pid_file.read_text().strip() or "0") if client_pid_file.exists() else 0
        try:
            client_status = Path(f"/proc/{client_pid}/status").read_text().splitlines()
            client_stat = Path(f"/proc/{client_pid}/stat").read_text().split(")", 1)[1].split()
            client_cpu = (int(client_stat[11]) + int(client_stat[12])) / os.sysconf("SC_CLK_TCK")
            client_memory = {line.split()[0]: int(line.split()[1]) * 1024 for line in client_status
                             if line.startswith(("VmRSS:", "VmHWM:", "VmSwap:"))}
            row.update({"client_pid": client_pid,
                        "client_cpu_pct_one_core": round((client_cpu - previous_client_cpu) / max(now - previous_time, .001) * 100, 2)
                            if client_pid == previous_client_pid else 0,
                        "client_rss_bytes": client_memory.get("VmRSS:", 0),
                        "client_peak_rss_bytes": client_memory.get("VmHWM:", 0),
                        "client_swap_bytes": client_memory.get("VmSwap:", 0)})
            previous_client_pid, previous_client_cpu = client_pid, client_cpu
        except FileNotFoundError:
            pass
        writer.writerow(row)
        output.flush()
        previous_time, previous_cpu = now, cpu["usage_usec"]
        time.sleep(2)
