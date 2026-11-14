"""Enable the one NAPI thread on a newly created, uniquely named private TAP."""

import argparse
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time


def matching_threads(name):
    listing = subprocess.check_output(["ps", "-e", "-o", "pid=,comm="], text=True)
    return [
        int(parts[0]) for line in listing.splitlines() if len(parts := line.split()) == 2 and parts[1] == name
    ]


def activate(device, cpu, namespace_inode, host_inode):
    live_inode = os.stat("/proc/self/ns/net").st_ino
    if live_inode != namespace_inode or live_inode == host_inode or not re.fullmatch(r"t[0-9a-f]{7}", device):
        raise ValueError("expected a unique fixture TAP in the designated private namespace")
    info = json.loads(subprocess.check_output(["ip", "-j", "-d", "link", "show", "dev", device], text=True))[
        0
    ]["linkinfo"]
    if info["info_kind"] != "tun" or info.get("info_data", {}).get("type") != "tap":
        raise ValueError(f"unexpected fixture TAP link: {info}")
    path = Path("/sys/class/net") / device
    (mask,) = path.glob("queues/rx-*/rps_cpus")
    # Linux 6.8 tun_napi_init uses netif_napi_add_tx, which intentionally
    # excludes this NAPI instance from the busy-poll ID namespace.
    name = f"napi/{device}-0"
    if matching_threads(name) or (path / "threaded").read_text().strip() != "0":
        raise RuntimeError("expected a fresh single-queue TAP and a unique new kernel task")
    mask.write_text("0\n")
    if int(mask.read_text().replace(",", "").strip(), 16):
        raise RuntimeError("private TAP RX steering remained enabled")
    (path / "threaded").write_text("1\n")
    deadline = time.monotonic() + 2
    tasks = matching_threads(name)
    while not tasks and time.monotonic() < deadline:
        time.sleep(0.015)
        tasks = matching_threads(name)
    if len(tasks) != 1:
        raise RuntimeError(f"could not identify unique private TAP thread {name}: {tasks}")
    (pid,) = tasks
    task = Path("/proc") / str(pid)
    if (task / "comm").read_text().strip() != name or not any(
        line.split() == ["Kthread:", "1"] for line in (task / "status").read_text().splitlines()
    ):
        raise RuntimeError("private TAP kernel thread failed its identity check")
    starttime = (task / "stat").read_text().rsplit(")", 1)[1].split()[19]
    os.sched_setaffinity(pid, {cpu})
    if os.sched_getaffinity(pid) != {cpu} or (path / "threaded").read_text().strip() != "1":
        raise RuntimeError("private TAP thread placement did not take effect")
    ifindex = socket.if_nametoindex(device)
    return {
        "namespace_inode": live_inode,
        "ifindex": ifindex,
        "device": device,
        "threaded": "1",
        "rx_rps_cpus": [mask.read_text().strip()],
        "napi": [
            {
                "id": 0,
                "ifindex": ifindex,
                "pid": pid,
                "comm": name,
                "cpus": [cpu],
                "starttime": starttime,
                "driver_registration": "netif_napi_add_tx",
            }
        ],
    }


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("device")
    p.add_argument("cpu", type=int)
    p.add_argument("namespace_inode", type=int)
    p.add_argument("host_inode", type=int)
    args = p.parse_args()
    print(json.dumps(activate(args.device, args.cpu, args.namespace_inode, args.host_inode)))
