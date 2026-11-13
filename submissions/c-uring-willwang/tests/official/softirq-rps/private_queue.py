"""Set or read the receive mask of one proven private, single-queue ordinary-NAPI TAP."""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import subprocess


def read(path):
    return path.read_text().strip()


def mask(cpu):
    value = 1 << cpu
    return ",".join(f"{value >> bit & 0xffffffff:08x}" for bit in reversed(range(0, value.bit_length(), 32)))


def observe(device, cpu, target_inode, launcher_inode, configure=False):
    live = os.stat("/proc/self/ns/net").st_ino
    if live != target_inode or live == launcher_inode or not re.fullmatch(r"t[0-9a-f]{7}", device):
        raise ValueError("expected the designated unique private fixture TAP")
    if cpu < 0 or not (Path("/sys/devices/system/cpu") / f"cpu{cpu}").is_dir():
        raise ValueError("unknown intended CPU")
    path = Path("/sys/class/net") / device
    link = json.loads(subprocess.check_output(["ip", "-j", "-d", "link", "show", "dev", device]))[0]
    linkinfo = link["linkinfo"]
    queues = sorted(q.name for q in (path / "queues").iterdir())
    if (linkinfo.get("info_kind"), linkinfo.get("info_data", {}).get("type"), queues) != ("tun", "tap", ["rx-0", "tx-0"]):
        raise ValueError("expected a single private TAP RX/TX queue")
    if (link["mtu"], link["txqlen"], link["num_rx_queues"], link["num_tx_queues"]) != (1500, 4096, 1, 1):
        raise ValueError("unexpected private TAP geometry")
    if int(read(path / "ifindex")) != socket.if_nametoindex(device) or link["ifindex"] != socket.if_nametoindex(device):
        raise ValueError("netlink and sysfs do not name the same TAP")
    rx = path / "queues/rx-0"
    rps = rx / "rps_cpus"
    before = read(rps)
    if configure:
        if read(path / "threaded") != "0" or int(read(rx / "rps_flow_cnt")) != 0:
            raise ValueError("expected a fresh ordinary-NAPI TAP with flow steering disabled")
        if int(before.replace(",", ""), 16) != 0:
            raise ValueError("fresh target already has a receive mask")
        rps.write_text(mask(cpu) + "\n")
    result = dict(device=device, ifindex=link["ifindex"], namespace_inode=live,
                  rx_rps_cpus=[read(rps)], rps_flow_cnt=int(read(rx / "rps_flow_cnt")),
                  threaded=read(path / "threaded"), socket_napi_id=0, napi=[])
    if configure:
        if (int(result["rx_rps_cpus"][0].replace(",", ""), 16) != 1 << cpu or
                result["threaded"] != "0" or result["rps_flow_cnt"]):
            raise ValueError("private single-CPU RPS setting failed readback")
        result["original_mask"] = before
    return result


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("device")
    p.add_argument("cpu", type=int)
    p.add_argument("target_inode", type=int)
    p.add_argument("launcher_inode", type=int)
    p.add_argument("--read", action="store_true")
    a = p.parse_args()
    print(json.dumps(observe(a.device, a.cpu, a.target_inode, a.launcher_inode, not a.read)))
