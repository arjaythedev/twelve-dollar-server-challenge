"""Enable and verify threaded NAPI on one fixture-owned veth inside a private network namespace."""

import argparse
import json
import os
from pathlib import Path
import socket
import struct
import subprocess


def aligned(n):
    return (n + 3) & ~3


def attr(kind, payload):
    length = 4 + len(payload)
    return struct.pack("=HH", length, kind) + payload + bytes(aligned(length) - length)


def attrs(data):
    off = 0
    while off + 4 <= len(data):
        length, kind = struct.unpack_from("=HH", data, off)
        if length < 4 or off + length > len(data):
            raise ValueError("malformed netlink attribute")
        yield kind & 0x3FFF, data[off + 4 : off + length]
        off += aligned(length)


def netlink(sock, family, command, attributes, sequence, dump=False):
    body = struct.pack("=BBH", command, 1, 0) + attributes
    sock.sendto(
        struct.pack("=IHHII", 16 + len(body), family, 0x301 if dump else 1, sequence, 0) + body, (0, 0)
    )
    replies = []
    while True:
        packet, sender = sock.recvfrom(65536)
        if sender[0] != 0:
            raise ValueError("unexpected netlink sender")
        off = 0
        while off + 16 <= len(packet):
            length, typ, flags, seq, _ = struct.unpack_from("=IHHII", packet, off)
            if length < 16 or off + length > len(packet):
                raise ValueError("malformed netlink frame")
            payload = packet[off + 16 : off + length]
            off += aligned(length)
            if seq != sequence:
                continue
            if typ == 3:
                return replies
            if typ == 2:
                err = struct.unpack_from("=i", payload)[0]
                if err:
                    raise OSError(-err, os.strerror(-err))
                continue
            replies.append(dict(attrs(payload[4:])))
            if not dump:
                return replies


def napi_records(ifindex):
    # netdev UAPI from Linux 6.8 include/uapi/linux/netdev.h.
    with socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, 16) as sock:
        sock.settimeout(3)
        sock.bind((0, 0))
        family_info = netlink(sock, 0x10, 3, attr(2, b"netdev\0"), 11)
        family = struct.unpack("=H", family_info[0][1])[0]
        data = netlink(sock, family, 11, attr(1, struct.pack("=I", ifindex)), 12, True)
    return [
        {
            k: struct.unpack("=I", row[tag])[0]
            for k, tag in (("ifindex", 1), ("id", 2), ("pid", 4))
            if tag in row
        }
        for row in data
        if row.get(1) == struct.pack("=I", ifindex)
    ]


def activate(device, cpu, namespace_inode, host_inode):
    current = os.stat("/proc/self/ns/net").st_ino
    if current != namespace_inode or current == host_inode:
        raise RuntimeError("refusing to configure a network namespace outside this fixture")
    link = json.loads(subprocess.check_output(["ip", "-j", "-d", "link", "show", "dev", device], text=True))[
        0
    ]
    if link["linkinfo"]["info_kind"] != "veth":
        raise ValueError("fixture device must be a veth")
    ifindex = socket.if_nametoindex(device)
    path = Path("/sys/class/net") / device
    prior = napi_records(ifindex)
    if not prior or any("pid" in row for row in prior) or (path / "threaded").read_text().strip() != "0":
        raise RuntimeError(f"expected newly created unthreaded NAPI entries: {prior}")
    rps = sorted(path.glob("queues/rx-*/rps_cpus"))
    if not rps:
        raise RuntimeError("no private RX queue")
    for mask in rps:
        mask.write_text("0\n")
        if int(mask.read_text().strip().replace(",", ""), 16):
            raise RuntimeError("failed to disable private RX steering")
    (path / "threaded").write_text("1\n")
    after = napi_records(ifindex)
    if {n["id"] for n in prior} != {n["id"] for n in after} or not all(n.get("pid") for n in after):
        raise RuntimeError(f"kernel did not publish every private NAPI kthread: {after}")
    for row in after:
        task = Path("/proc") / str(row["pid"])
        comm = (task / "comm").read_text().strip()
        status = (task / "status").read_text().splitlines()
        expected = f"napi/{device}-{row['id']}"
        if comm not in (expected, expected[:15]) or not any(
            line.split() == ["Kthread:", "1"] for line in status
        ):
            raise RuntimeError(f"kernel task identity check failed for {row}, {comm}")
        os.sched_setaffinity(row["pid"], {cpu})
        if os.sched_getaffinity(row["pid"]) != {cpu}:
            raise RuntimeError("NAPI thread affinity did not take effect")
        row["comm"] = comm
        row["cpus"] = sorted(os.sched_getaffinity(row["pid"]))
        row["starttime"] = (task / "stat").read_text().rsplit(")", 1)[1].split()[19]
    return {
        "namespace_inode": current,
        "device": device,
        "ifindex": ifindex,
        "rx_rps_cpus": [p.read_text().strip() for p in rps],
        "threaded": (path / "threaded").read_text().strip(),
        "napi": after,
    }


if __name__ == "__main__":
    args = argparse.ArgumentParser()
    args.add_argument("device")
    args.add_argument("cpu", type=int)
    args.add_argument("namespace_inode", type=int)
    args.add_argument("host_inode", type=int)
    a = args.parse_args()
    print(json.dumps(activate(a.device, a.cpu, a.namespace_inode, a.host_inode)))
