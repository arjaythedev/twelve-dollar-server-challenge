"""Probe one disposable veth pair; no benchmark interface or server is involved."""

import json
import argparse
import os
from pathlib import Path
import subprocess
import time
import uuid

here = Path(__file__).resolve().parent
suffix = uuid.uuid4().hex[:4]
server = "tnps" + suffix
client = "tnpc" + suffix
device = "np" + suffix
peer = "tx" + suffix
parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
parser.add_argument("--app-cpu", type=int, default=9)
parser.add_argument("--sender-cpu", type=int, default=15)
args = parser.parse_args()
cpu = args.app_cpu
sender_cpu = args.sender_cpu
if cpu == sender_cpu or not {cpu, sender_cpu} <= os.sched_getaffinity(0):
    parser.error("choose two distinct CPUs allowed for this proof")
host_inode = os.stat("/proc/self/ns/net").st_ino
result = {"host_inode": host_inode, "controller_cpu": cpu, "sender_cpu": sender_cpu, "trials": []}
created = []
threads = []


def cmd(*args):
    v = subprocess.run(["sudo", "-n", *map(str, args)], text=True, capture_output=True, timeout=8)
    if v.returncode:
        raise RuntimeError(f"{args}: {v.stderr} {v.stdout}")
    return v.stdout.strip()


def ns(name, *args):
    return cmd("ip", "netns", "exec", name, *args)


def unpriv(nsname, cpunum, side, phase):
    return [
        "sudo",
        "-n",
        "ip",
        "netns",
        "exec",
        nsname,
        "taskset",
        "-c",
        str(cpunum),
        "setpriv",
        f"--reuid={os.getuid()}",
        f"--regid={os.getgid()}",
        "--init-groups",
        "--inh-caps=-all",
        "--ambient-caps=-all",
        "--bounding-set=-all",
        "--no-new-privs",
        "/usr/bin/python3",
        str(here / "socket_probe.py"),
        side,
        phase,
    ]


def thread_counters(rows):
    return {
        str(t["pid"]): [int(v) for v in Path(f"/proc/{t['pid']}/schedstat").read_text().split()] for t in rows
    }


def kthread_survives(row):
    path = Path(f"/proc/{row['pid']}/stat")
    try:
        return path.read_text().rsplit(")", 1)[1].split()[19] == row["starttime"]
    except FileNotFoundError:
        return False


try:
    os.sched_setaffinity(0, {cpu})
    for name in (server, client):
        cmd("ip", "netns", "add", name)
        created.append(name)
        cmd("ip", "-n", name, "link", "set", "lo", "up")
    cmd(
        "ip",
        "-n",
        server,
        "link",
        "add",
        "name",
        device,
        "numrxqueues",
        "1",
        "numtxqueues",
        "1",
        "type",
        "veth",
        "peer",
        "name",
        peer,
        "numrxqueues",
        "1",
        "numtxqueues",
        "1",
        "netns",
        client,
    )
    for name, dev, address in ((server, device, "10.232.80.1/30"), (client, peer, "10.232.80.2/30")):
        cmd("ip", "-n", name, "address", "add", address, "dev", dev)
        cmd("ip", "-n", name, "link", "set", dev, "mtu", "1500", "up")
        ns(name, "ethtool", "-K", dev, "gro", "on", "gso", "on")
    namespace_inode = os.stat(f"/run/netns/{server}").st_ino
    result["activation"] = json.loads(
        ns(server, "/usr/bin/python3", here / "private_napi.py", device, cpu, namespace_inode, host_inode)
    )
    threads = result["activation"]["napi"]
    for phase in ("tso-on", "tso-off"):
        ns(client, "ethtool", "-K", peer, "tso", phase.split("-")[1])
        vals = ns(client, "ethtool", "-k", peer)
        before = thread_counters(threads)
        app = subprocess.Popen(
            unpriv(server, cpu, "server", phase), text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        try:
            if app.stdout.readline().strip() != "READY":
                raise RuntimeError("isolated receiver did not start")
            subprocess.run(unpriv(client, sender_cpu, "client", phase), check=True, timeout=4)
            stdout, stderr = app.communicate(timeout=3)
            if app.returncode:
                raise RuntimeError(stderr)
        finally:
            if app.poll() is None:
                app.terminate()
                app.wait(timeout=2)
        after = thread_counters(threads)
        details = json.loads(stdout)
        details["peer_features"] = [
            s.strip()
            for s in vals.splitlines()
            if "tcp-segmentation" in s or "tx-tcp" in s or "generic-segmentation" in s
        ]
        details["napi_runtime_ns"] = {pid: after[pid][0] - v[0] for pid, v in before.items()}
        result["trials"].append(details)
finally:
    for name in reversed(created):
        cmd("ip", "netns", "del", name)
    deadline = time.monotonic() + 5
    while any(kthread_survives(r) for r in threads) and time.monotonic() < deadline:
        time.sleep(0.03)
    result["kernel_threads_reclaimed"] = not any(kthread_survives(r) for r in threads)
    result["namespaces_reclaimed"] = not any(Path("/run/netns", name).exists() for name in created)
    text = json.dumps(result, indent=2) + "\n"
    args.output.write_text(text)
    print(text)
