"""Low-frequency passive host counters for the server namespace and the trial's scheduler CPUs."""

import json
from itertools import islice
import os
from pathlib import Path
import sys
import time


def sample_socket_queues(pid, limit=1024):
    # Stop at a bounded prefix of hash-distributed rows. A procfs scan of every
    # connection would contend with the service's socket locks at high load.
    totals = {
        "rows_seen": 0,
        "established": 0,
        "close_wait": 0,
        "rx_queued_sockets": 0,
        "rx_payload_bytes": 0,
        "tx_queued_sockets": 0,
        "tx_payload_bytes": 0,
    }
    try:
        with (Path("/proc") / str(pid) / "net/tcp").open() as fp:
            next(fp, None)
            for line in islice(fp, limit):
                cols = line.split()
                totals["rows_seen"] += 1
                if len(cols) < 5 or not cols[1].endswith(":0050"):
                    continue
                if cols[3] == "08":
                    totals["close_wait"] += 1
                if cols[3] != "01":
                    continue
                totals["established"] += 1
                tx, rx = (int(v, 16) for v in cols[4].split(":"))
                totals["rx_queued_sockets"] += bool(rx)
                totals["rx_payload_bytes"] += rx
                totals["tx_queued_sockets"] += bool(tx)
                totals["tx_payload_bytes"] += tx
    except (PermissionError, FileNotFoundError, ProcessLookupError):
        return {}
    return totals


def pressure(path):
    return {
        line.split()[0]: {
            k: float(v) if k != "total" else int(v) for k, v in (part.split("=") for part in line.split()[1:])
        }
        for line in path.read_text().splitlines()
    }


def host_tcp_counters():
    host_tcp = next(
        line for line in Path("/proc/net/sockstat").read_text().splitlines() if line.startswith("TCP:")
    )
    values = host_tcp.split()[1:]
    return {k: int(v) for k, v in zip(values[::2], values[1::2])}


def napi_scheduling(pids):
    counters = {}
    for pid in pids:
        try:
            data = (Path("/proc") / str(pid) / "schedstat").read_text().split()
            counters[str(pid)] = dict(zip(("run_ns", "wait_ns", "timeslices"), map(int, data)))
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
    return counters


def snapshot(pid, cg, napi_pids=()):
    keys = ("user", "nice", "sys", "idle", "iowait", "irq", "softirq", "steal")
    out = {
        "unix": time.time(),
        "cpus": {
            line.split()[0]: dict(zip(keys, map(int, line.split()[1:])))
            for line in Path("/proc/stat").read_text().splitlines()
            if line.startswith("cpu") and len(line.split()[0]) > 3
        },
        "pressure": {},
    }
    counters = (
        "anon",
        "file",
        "sock",
        "kernel",
        "slab",
        "slab_reclaimable",
        "slab_unreclaimable",
        "active_file",
        "inactive_file",
        "shmem",
        "unevictable",
        "file_dirty",
        "file_writeback",
        "workingset_refault_anon",
        "workingset_refault_file",
        "workingset_activate_file",
        "pgscan",
        "pgsteal",
        "pgmajfault",
    )
    out["softnet"] = {
        str(i): dict(zip(("processed", "dropped", "time_squeeze"), [int(v, 16) for v in line.split()[:3]]))
        for i, line in enumerate(Path("/proc/net/softnet_stat").read_text().splitlines())
    }
    for name in ("cpu", "io", "memory"):
        try:
            out["pressure"][name] = pressure(cg / (name + ".pressure"))
        except FileNotFoundError:
            pass
    try:
        out["memory"] = {
            k: int(v)
            for k, v in (line.split() for line in (cg / "memory.stat").read_text().splitlines())
            if k in counters
        }
        out["service"] = {
            key: int((cg / key).read_text())
            for key in ("memory.peak", "memory.current", "memory.swap.current")
        }
        out["memory_events"] = {
            k: int(v) for k, v in (l.split() for l in (cg / "memory.events").read_text().splitlines())
        }
    except FileNotFoundError:
        pass
    for file, group, names in (
        ("snmp", "Tcp", {"CurrEstab", "InSegs", "OutSegs", "RetransSegs", "EstabResets", "AttemptFails"}),
        (
            "netstat",
            "TcpExt",
            {
                "ListenDrops",
                "ListenOverflows",
                "TCPBacklogDrop",
                "TCPTimeouts",
                "TCPRcvQDrop",
                "TCPOrigDataSent",
                "DelayedACKs",
                "TCPHPAcks",
                "TCPPureAcks",
                "TCPAckCompressed",
                "TCPRcvCoalesce",
                "TCPMemoryPressures",
                "TCPSynRetrans",
                "SyncookiesSent",
            },
        ),
    ):
        try:
            lines = (Path("/proc") / str(pid) / "net" / file).read_text().splitlines()
            for head, vals in zip(lines[::2], lines[1::2]):
                if head.startswith(group + ":"):
                    out[group] = {k: int(v) for k, v in zip(head.split()[1:], vals.split()[1:]) if k in names}
        except (PermissionError, FileNotFoundError, ProcessLookupError):
            pass
    fields = (
        "reads",
        "reads_merged",
        "sectors_read",
        "read_ms",
        "writes",
        "writes_merged",
        "sectors_written",
        "write_ms",
        "in_flight",
        "busy_ms",
        "weighted_busy_ms",
        "discards",
        "discards_merged",
        "sectors_discarded",
        "discard_ms",
        "flushes",
        "flush_ms",
    )
    out["diskstats"] = {}
    for line in Path("/proc/diskstats").read_text().splitlines():
        major, minor, device, *values = line.split()
        if not device.startswith(("loop", "ram", "zram")):
            out["diskstats"][device] = dict(zip(fields, map(int, values)))
    out["host_io_pressure"] = pressure(Path("/proc/pressure/io"))
    out["host_sockstat_tcp"] = host_tcp_counters()
    if napi_pids:
        out["napi_threads"] = napi_scheduling(napi_pids)
    try:
        out["io_stat"] = {}
        for line in (cg / "io.stat").read_text().splitlines():
            device, *metrics = line.split()
            out["io_stat"][device] = {
                k: int(v)
                for k, v in (f.split("=", 1) for f in metrics)
                if k in ("rbytes", "wbytes", "rios", "wios", "dbytes", "dios", "rusec", "wusec", "dusec")
            }
    except FileNotFoundError:
        pass
    return out


def main():
    out = Path(sys.argv[1]).resolve()
    app = json.loads((out / "app-start.json").read_text())
    pid = app["pid"]
    cg = Path("/sys/fs/cgroup") / app["cgroup"].split("::", 1)[1].lstrip("/")
    os.sched_setaffinity(0, {15})
    with (out / "telemetry.jsonl").open("x") as fp:
        while cg.exists() and not (out / "summary.json").exists():
            print(json.dumps(snapshot(pid, cg)), file=fp, flush=True)
            time.sleep(2)


if __name__ == "__main__":
    main()
