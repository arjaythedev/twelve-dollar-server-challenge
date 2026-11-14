"""Run the same four stock generators over a single guest-facing TAP receive queue."""

import argparse
import fcntl
import json
import math
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import sys
import tempfile
import time
import uuid

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "router"))
import run as base

sys.path.insert(0, str(HERE.parent / "threaded-napi"))
from run_threaded import still_alive, verify_wire


class TapFixture:
    def __init__(self, binary):
        self.binary = binary.resolve()
        self.process = None
        self.pid = None
        self.log = None
        self.out = None
        self.napi = []
        self.interfaces = {}

    def start(self, cpu, delay, endpoints):
        if not self.binary.is_file() or not os.access(self.binary, os.X_OK):
            raise RuntimeError(
                f"build the fixture's relay.c and set TWELVE_TAP_RELAY to its executable: {self.binary}"
            )
        self.log = (self.out / "tap-relay.log").open("w")
        self.process = subprocess.Popen(
            [
                "sudo",
                "-n",
                "taskset",
                "-c",
                str(cpu),
                str(self.binary),
                str(os.getuid()),
                str(os.getgid()),
                str(delay),
                str(self.out / "tap-relay-current.json"),
                *[f"/run/netns/{n}" for n in endpoints],
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.log,
            text=True,
            start_new_session=True,
        )
        if not select.select([self.process.stdout], [], [], 5)[0]:
            raise RuntimeError("private TAP creation timed out")
        line = self.process.stdout.readline().strip()
        if not re.fullmatch(r"READY \d+", line):
            raise RuntimeError(f"private TAP creation failed: {(self.out / 'tap-relay.log').read_text()}")
        self.pid = int(line.split()[1])
        if os.sched_getaffinity(self.pid) != {cpu} or (
            Path(f"/proc/{self.pid}").stat().st_uid != os.getuid()
        ):
            raise RuntimeError("relay CPU or ordinary-user privilege readback failed")

    def sample(self, _namespace):
        path = self.out / "tap-relay-current.json"
        if self.process.poll() is not None:
            raise RuntimeError(f"isolated polling relay exited with {self.process.returncode}")
        relay = json.loads(path.read_text())
        relay["sample_age_s"] = round(time.time() - path.stat().st_mtime, 3)
        links = {}
        for name, dev in self.interfaces.items():
            row = json.loads(base.ns(name, "ip", "-j", "-s", "link", "show", "dev", dev))[0]
            stats = row.get("stats64", row.get("stats", {}))
            links[name] = {
                direction: {k: stats[direction].get(k, 0) for k in ("packets", "dropped", "errors")}
                for direction in ("rx", "tx")
            }
        return {
            "polling_relay": relay,
            "tap_interfaces": links,
            "router_rx": {},
            "router_tx": {},
            "router_queues": {},
        }

    def set_up(self, a, names, created):
        self.out = a.output.resolve() / a.tag
        server, parent, *clients = names
        base.priv("ip", "netns", "add", parent)
        created.append(parent)
        base.ns(parent, "sysctl", "-q", "-w", f"net.ipv4.tcp_child_ehash_entries={a.server_ehash}")
        base.priv("nsenter", f"--net=/run/netns/{parent}", "--", "ip", "netns", "add", server)
        created.append(server)
        base.ns(parent, "sysctl", "-q", "-w", "net.ipv4.tcp_child_ehash_entries=0")
        for ns, group in zip(clients, a.groups):
            base.priv("ip", "netns", "add", ns)
            created.append(ns)
            mask = sum(1 << int(c) for c in group.split(","))
            base.ns(ns, "sysctl", "-q", "-w", f"net.core.rps_default_mask={base.cpu_mask(mask)}")
        for ns in names:
            base.ip(ns, "link", "set", "lo", "up")
        relay_cpu = a.router_cpus[-1]
        endpoints = [server, *clients]
        self.start(relay_cpu, a.delay_us, endpoints)
        service_dev = "t" + uuid.uuid4().hex[:7]
        base.ip(server, "link", "set", "labtap", "name", service_dev)
        topology = {}
        for i, ns in enumerate(names):
            topology[ns] = {
                "ehash_entries": base.ns(ns, "cat", "/proc/sys/net/ipv4/tcp_ehash_entries"),
                "ephemeral": base.ns(ns, "sysctl", "-n", "net.ipv4.ip_local_port_range"),
                "rx_masks": {},
                "offloads": {},
            }
        if a.server_ehash and int(topology[server]["ehash_entries"]) != a.server_ehash:
            raise RuntimeError("server did not acquire its private established socket hash")
        for idx, ns in enumerate(endpoints):
            dev = service_dev if ns == server else "labtap"
            self.interfaces[ns] = dev
            source = "10.232.255.1/16" if ns == server else f"10.232.{idx}.254/16"
            base.ip(ns, "link", "set", dev, "address", f"02:12:0d:00:00:{idx+1:02x}")
            base.ip(ns, "address", "add", source, "dev", dev)
            base.ip(ns, "link", "set", dev, "mtu", "1500", "up")
            link = json.loads(base.ns(ns, "ip", "-j", "-d", "link", "show", "dev", dev))[0]
            geometry = {key: link.get(key) for key in ("mtu", "num_rx_queues", "num_tx_queues", "txqlen")}
            if (geometry["mtu"], geometry["num_rx_queues"], geometry["num_tx_queues"]) != (1500, 1, 1):
                raise RuntimeError(f"private TAP queue count or MTU did not match: {geometry}")
            topology[ns]["interface"] = geometry
            base.ns(ns, "ethtool", "-K", dev, "gro", "on", "gso", "on")
            features = {}
            for key, value in re.findall(
                r"^(generic-receive-offload|generic-segmentation-offload|tcp-segmentation-offload|tx-checksumming): (on|off)",
                base.ns(ns, "ethtool", "-k", dev),
                re.M,
            ):
                features[key] = value
            if set(features.values()) != {"on"} or len(features) != 4:
                raise RuntimeError(f"TAP offload configuration mismatch: {features}")
            topology[ns]["offloads"][dev] = features
            topology[ns]["rx_masks"][dev] = base.ns(ns, "cat", f"/sys/class/net/{dev}/queues/rx-0/rps_cpus")
            if ns != server:
                mask = sum(1 << int(c) for c in a.groups[idx - 1].split(","))
                if int(topology[ns]["rx_masks"][dev].replace(",", ""), 16) != mask:
                    raise RuntimeError("client RX CPU steering did not take effect")
                for host in range(10, 10 + a.aliases):
                    base.ip(server, "address", "add", f"10.232.{idx}.{host}/32", "dev", service_dev)
        detail = json.loads(
            base.ns(
                server,
                "/usr/bin/python3",
                HERE / "tap_napi.py",
                service_dev,
                a.app_cpu,
                os.stat(f"/run/netns/{server}").st_ino,
                os.stat("/proc/self/ns/net").st_ino,
            )
        )
        self.napi = detail["napi"]
        topology[server]["rx_masks"][service_dev] = detail["rx_rps_cpus"][0]
        topology[server]["threaded_napi"] = {service_dev: detail}
        self.process.stdin.write("G")
        self.process.stdin.flush()
        topology[parent]["polling_relay"] = {
            "cpu": relay_cpu,
            "uid": os.getuid(),
            "pid": self.pid,
            "mtu": 1500,
            "server_tap": service_dev,
            "delay_us": a.delay_us,
            "ports": endpoints,
            "binary_sha256": base.sha(self.binary),
        }
        for idx, (ns, group) in enumerate(zip(clients, a.groups), 1):
            lo, hi = map(int, topology[ns]["ephemeral"].split())
            capacity = a.aliases * (hi - lo + 1)
            if capacity < math.ceil(a.users / len(clients)):
                raise ValueError("insufficient source and destination tuples")
            topology[ns]["source_destination_tuple_capacity"] = capacity
            addr = f"10.232.{idx}.10"
            topology[ns]["socket_probe_cpu_and_napi_id"] = verify_wire(
                server, ns, a.app_cpu, group, addr, self.napi
            )
            topology[ns]["ping"] = base.ns(
                ns, "taskset", "-c", group, "ping", "-n", "-D", "-i", "0.02", "-c", "20", "-W", "1", addr
            )
            if not re.search(r"\b20 received, 0% packet loss", topology[ns]["ping"]):
                raise RuntimeError("private link did not carry all unloaded probes")
        return topology

    def close(self):
        if self.process and self.process.poll() is None:
            if self.pid:
                try:
                    os.kill(self.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            else:
                self.process.terminate()
            self.process.wait(timeout=4)
        if self.log:
            self.log.close()
        if self.napi:
            deadline = time.monotonic() + 6
            while any(still_alive(row) for row in self.napi) and time.monotonic() < deadline:
                time.sleep(0.05)
            survivors = [r for r in self.napi if still_alive(r)]
            base.write(
                self.out / "tap-cleanup.json",
                {"created_threads": len(self.napi), "unreclaimed_threads": survivors},
            )
            if survivors:
                raise RuntimeError(f"private TAP kernel thread survived cleanup: {survivors}")


def small_proof(fixture):
    p = argparse.ArgumentParser()
    p.add_argument("--wire-proof-only", action="store_true")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--delay-us", type=int, default=90)
    p.add_argument("--app-cpu", type=int, default=9)
    p.add_argument("--relay-cpu", type=int, default=15)
    a = p.parse_args()
    a.output = a.output.resolve()
    a.output.mkdir(parents=True, exist_ok=False)
    a.output, a.tag = a.output.parent, a.output.name
    a.server_ehash, a.aliases, a.users = 16384, 8, 16
    a.router_cpus, a.groups = [a.relay_cpu], [str(a.app_cpu)] * 4
    os.sched_setaffinity(0, {a.app_cpu})
    lock = Path(
        os.environ.get(
            "TWELVE_WIRE_PROOF_LOCK",
            str(Path(tempfile.gettempdir()) / f"twelve-wire-proof-{os.getuid()}-cpu{a.app_cpu}.lock"),
        )
    )
    gate = lock.open("a")
    fcntl.flock(gate, fcntl.LOCK_EX)
    short = "tt" + uuid.uuid4().hex[:7]
    names = [short + suffix for suffix in ("s", "p", "c0", "c1", "c2", "c3")]
    created = []
    try:
        data = fixture.set_up(a, names, created)
        base.write(a.output / a.tag / "shape.json", data)
        print(
            json.dumps(
                {
                    n: [float(v) for v in re.findall(r"time[=<]([\d.]+) ms", o["ping"])]
                    for n, o in data.items()
                    if "ping" in o
                }
            ),
            flush=True,
        )
    finally:
        for name in reversed(created):
            base.priv("ip", "netns", "del", name, check=False)


if __name__ == "__main__":
    binary = Path(os.environ.get("TWELVE_TAP_RELAY", str(HERE / "relay")))
    fixture = TapFixture(binary)
    try:
        if "--wire-proof-only" in sys.argv:
            small_proof(fixture)
        else:
            base.main(
                network_setup=fixture.set_up,
                network_profile="single-queue-polling-tap",
                network_sample=fixture.sample,
                delay_us=90,
            )
    finally:
        fixture.close()
