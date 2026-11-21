"""Measure the drop reason when a private, explicitly detached-from-traffic TAP reader is held idle."""

import argparse
import collections
import fcntl
import json
import os
from pathlib import Path
import re
import select
import subprocess
import sys
import tempfile
import uuid

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "router"))
import run as h


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out", type=Path)
    parser.add_argument("--txqlen", type=int, choices=(1000, 4096), default=1000)
    parser.add_argument("--sender-cpu", type=int, default=9)
    parser.add_argument("--relay-cpu", type=int, default=15)
    args = parser.parse_args()
    if os.getuid() == 0 or args.sender_cpu == args.relay_cpu:
        parser.error("use an ordinary benchmark user and two distinct spare CPUs")
    if not {args.sender_cpu, args.relay_cpu} <= os.sched_getaffinity(0):
        parser.error("both proof CPUs must be in this process's allowed CPU set")
    frames = args.txqlen + 100
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    gate = Path(
        os.environ.get(
            "TWELVE_WIRE_PROOF_LOCK",
            str(Path(tempfile.gettempdir()) / f"twelve-wire-proof-{os.getuid()}-cpu{args.sender_cpu}.lock"),
        )
    ).open("a")
    fcntl.flock(gate, fcntl.LOCK_EX)
    suffix = uuid.uuid4().hex[:7]
    names = ["dr" + suffix + "s", "dr" + suffix + "c"]
    created = []
    relay = client = perf = None
    ctl = ack = None
    staff = []
    try:
        for n in names:
            h.priv("ip", "netns", "add", n)
            created.append(n)
        binary = Path(os.environ.get("TWELVE_TAP_RELAY", str(HERE.parent / "tap-poll" / "relay")))
        relay = subprocess.Popen(
            [
                "sudo",
                "-n",
                "taskset",
                "-c",
                str(args.relay_cpu),
                str(binary),
                str(os.getuid()),
                str(os.getgid()),
                "0",
                str(out / "relay.json"),
                *[f"/run/netns/{n}" for n in names],
            ],
            text=True,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if not select.select([relay.stdout], [], [], 5)[0]:
            raise RuntimeError("private TAP startup timed out")
        ready = relay.stdout.readline().strip()
        if not ready.startswith("READY "):
            raise RuntimeError(ready)
        n = names[0]
        dev = "labtap"
        h.ns(n, "sysctl", "-q", "-w", f"net.ipv6.conf.{dev}.disable_ipv6=1")
        (initial_link,) = json.loads(h.ns(n, "ip", "-j", "-d", "link", "show", "dev", dev))
        if (
            initial_link["linkinfo"]["info_kind"],
            initial_link["linkinfo"]["info_data"]["type"],
            initial_link["txqlen"],
            initial_link["num_tx_queues"],
        ) != ("tun", "tap", 1000, 1):
            raise RuntimeError("expected one new private TAP with the default transmit ring")
        if args.txqlen != 1000:
            h.ip(n, "link", "set", "dev", dev, "txqueuelen", args.txqlen)
        h.ip(n, "link", "set", "lo", "up")
        h.ip(n, "link", "set", dev, "address", "02:12:0d:00:00:01", "mtu", "1500", "up")
        h.ip(n, "addr", "add", "10.232.80.1/30", "dev", dev)
        h.ip(
            n,
            "neigh",
            "replace",
            "10.232.80.2",
            "lladdr",
            "02:12:0d:00:00:02",
            "nud",
            "permanent",
            "dev",
            dev,
        )

        def counters():
            z = json.loads(h.ns(n, "ip", "-j", "-d", "-s", "link", "show", "dev", dev))[0]
            return {
                "tx": z["stats64"]["tx"],
                "txqlen": z["txqlen"],
                "qdisc": json.loads(h.ns(n, "tc", "-j", "-s", "qdisc", "show", "dev", dev)),
            }

        before = counters()
        if before["txqlen"] != args.txqlen:
            raise RuntimeError("private TAP transmit ring has the wrong capacity")
        script = """\
import os
import socket
import sys

with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
    print(os.getpid(), flush=True)
    sys.stdin.readline()
    errors = 0
    for i in range(int(sys.argv[1])):
        try:
            sender.sendto(i.to_bytes(4, "big"), ("10.232.80.2", 33819))
        except OSError:
            errors += 1
    print(errors, flush=True)
    sys.stdin.readline()
"""
        client = subprocess.Popen(
            [
                "sudo",
                "-n",
                "ip",
                "netns",
                "exec",
                n,
                "taskset",
                "-c",
                str(args.sender_cpu),
                "setpriv",
                f"--reuid={os.getuid()}",
                f"--regid={os.getgid()}",
                "--init-groups",
                "--inh-caps=-all",
                "--ambient-caps=-all",
                "--bounding-set=-all",
                "--no-new-privs",
                "/usr/bin/python3",
                "-c",
                script,
                str(frames),
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if not select.select([client.stdout], [], [], 4)[0]:
            raise RuntimeError("private sender startup timed out")
        pid = int(client.stdout.readline())
        cp = out / "perf-ctl"
        ap = out / "perf-ack"
        os.mkfifo(cp)
        os.mkfifo(ap)
        ctl = os.open(cp, os.O_RDWR | os.O_NONBLOCK)
        ack = os.open(ap, os.O_RDWR | os.O_NONBLOCK)
        log = (out / "perf.log").open("w")
        staff.append(log)
        perf = subprocess.Popen(
            [
                "sudo",
                "-n",
                "perf",
                "record",
                "-q",
                "--no-buildid",
                "--no-buildid-cache",
                "-D",
                "-1",
                "--control",
                f"fifo:{cp},{ap}",
                "-e",
                "skb:kfree_skb",
                "-p",
                str(pid),
                "-o",
                str(out / "drops.perf"),
            ],
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        os.write(ctl, b"enable\n")
        if not select.select([ack], [], [], 6)[0] or b"ack" not in os.read(ack, 128):
            raise RuntimeError("drop trace did not arm: " + (out / "perf.log").read_text())
        before = counters()
        client.stdin.write("go\n")
        client.stdin.flush()
        if not select.select([client.stdout], [], [], 4)[0]:
            raise RuntimeError("private sender did not finish")
        errors = int(client.stdout.readline())
        os.write(ctl, b"stop\n")
        perf.wait(timeout=5)
        trace_events = h.priv("perf", "script", "-i", out / "drops.perf")
        cases = collections.Counter()
        for line in trace_events.splitlines():
            if "skb:kfree_skb:" not in line:
                continue
            m = re.search(r"location=(\S+)", line)
            r = re.search(r"reason[:=]\s*(\S+)", line)
            cases[(m[1] if m else "unknown", r[1] if r else "unknown")] += 1
        after = counters()
        summary = {
            "initial_txqlen": initial_link["txqlen"],
            "tested_txqlen": args.txqlen,
            "attempted_udp_frames": frames,
            "syscall_errors": errors,
            "before": before,
            "after": after,
            "observed_drop_events": [
                {"site": k[0], "reason": k[1], "count": v} for k, v in cases.most_common()
            ],
        }
        h.write(out / "proof.json", summary)
        total = after["tx"]["dropped"] - before["tx"]["dropped"]
        if (
            errors
            or total != frames - args.txqlen
            or any(q.get("drops") for q in after["qdisc"])
            or sum(
                n for (site, reason), n in cases.items() if "tun_net_xmit" in site and "FULL_RING" in reason
            )
            != total
        ):
            raise RuntimeError(f"private TAP ring proof inconclusive: {cases}, interface delta {total}")
        print(json.dumps({"tx_drop": total, "qdisc_drop": 0, "drop_events": summary["observed_drop_events"]}))
    finally:
        if client and client.stdin:
            client.stdin.close()
        if perf and perf.poll() is None:
            perf.terminate()
        if client and client.poll() is None:
            client.wait(timeout=3)
        if relay and relay.stdin:
            relay.stdin.close()
        if relay and relay.poll() is None:
            relay.wait(timeout=3)
        for f in staff:
            f.close()
        for fd in (ctl, ack):
            if fd is not None:
                os.close(fd)
        for n in reversed(created):
            h.priv("ip", "netns", "del", n, check=False)


if __name__ == "__main__":
    main()
