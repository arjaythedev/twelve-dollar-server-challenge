"""Serve one guest TAP through independent, externally pinned per-source polling workers."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import select
import subprocess
import sys
import tempfile
import time
import uuid

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tap-poll"))
from run_tap import TapFixture, base

PROFILE = "single-queue-sharded-polling-tap"


def flow_process(namespace, core, role, address, count):
    return subprocess.Popen(
        [
            "sudo",
            "-n",
            "ip",
            "netns",
            "exec",
            namespace,
            "taskset",
            "-c",
            str(core),
            "setpriv",
            f"--reuid={os.getuid()}",
            f"--regid={os.getgid()}",
            "--init-groups",
            "--inh-caps=-all",
            "--ambient-caps=-all",
            "--bounding-set=-all",
            "--no-new-privs",
            "/usr/bin/python3",
            str(HERE.parent / "tap-poll" / "check_flow.py"),
            role,
            address,
            str(count),
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def verify_payloads(server, clients, args, count):
    servers, readers = [], []
    try:
        for i in range(len(clients)):
            servers.append(flow_process(server, args.app_cpu, "server", f"10.232.{i+1}.10", count))
        for process in servers:
            if not select.select([process.stdout], [], [], 5)[0] or not process.stdout.readline().startswith(
                "READY "
            ):
                raise RuntimeError("private route-proof server did not start")
        for i, client in enumerate(clients):
            readers.append(flow_process(client, args.groups[i], "client", f"10.232.{i+1}.10", count))
        records = []
        for sender, reader in zip(servers, readers):
            r_out, r_err = reader.communicate(timeout=8)
            s_out, s_err = sender.communicate(timeout=8)
            if sender.returncode or reader.returncode:
                raise RuntimeError(f"TCP payload proof failed: {s_err} {r_err}")
            observed, delivered = json.loads(s_out), json.loads(r_out)
            if (
                observed["cpus"] != [args.app_cpu]
                or observed["napi_ids"] != [0]
                or observed["mss"] > 1460
                or delivered["count"] != count
            ):
                raise RuntimeError(f"private TCP placement or segment size mismatch: {observed}, {delivered}")
            records.append({"service": observed, "client": delivered})
        if len({r["client"]["sha256"] for r in records}) != 1:
            raise RuntimeError("four private sockets received different expected payloads")
        return records
    finally:
        for process in servers + readers:
            if process.poll() is None:
                process.terminate()
        for process in servers + readers:
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()


class ShardedTap(TapFixture):
    def start(self, _last_cpu, delay, endpoints):
        if not self.binary.is_file() or not os.access(self.binary, os.X_OK):
            raise RuntimeError(
                f"build this fixture's relay.c and set TWELVE_SHARDED_TAP_RELAY to the executable: {self.binary}"
            )
        self.log = (self.out / "tap-relay.log").open("w")
        cpus = ",".join(map(str, self.external_cpus))
        self.process = subprocess.Popen(
            [
                "sudo",
                "-n",
                "taskset",
                "-c",
                cpus,
                str(self.binary),
                str(os.getuid()),
                str(os.getgid()),
                str(delay),
                str(self.out / "tap-relay-current.json"),
                cpus,
                *[f"/run/netns/{n}" for n in endpoints],
            ],
            text=True,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.log,
            start_new_session=True,
        )
        if not select.select([self.process.stdout], [], [], 5)[0]:
            raise RuntimeError("private sharded TAP creation timed out")
        line = self.process.stdout.readline().strip()
        if not re.fullmatch(r"READY \d+", line):
            raise RuntimeError(
                f"private sharded TAP startup failed: {(self.out / 'tap-relay.log').read_text()}"
            )
        self.pid = int(line.split()[1])
        if (
            os.sched_getaffinity(self.pid) != set(self.external_cpus)
            or Path(f"/proc/{self.pid}").stat().st_uid != os.getuid()
        ):
            raise RuntimeError(
                "polling-wire control process affinity or ordinary-user identity was incorrect"
            )

    def set_up(self, args, names, created):
        self.external_cpus = list(args.router_cpus)
        topology = super().set_up(args, names, created)
        config = topology[names[1]]["polling_relay"]
        config["cpus"] = self.external_cpus
        config["architecture"] = (
            "one reader per source TAP, ordered per-source delay queues, one guest RX and TX queue"
        )
        config["tcp_payload_proofs"] = verify_payloads(names[0], names[2:], args, 32)
        deadline = time.monotonic() + 4
        while True:
            stats = self.sample(names[1])["polling_relay"]
            workers = stats.get("workers", [])
            if len(workers) == len(self.external_cpus) and all(
                worker["loops"] and worker["forwarded_events"] for worker in workers
            ):
                break
            if time.monotonic() >= deadline:
                raise RuntimeError(f"sharded relay workers did not all forward frames: {workers}")
            time.sleep(0.1)
        if {w["cpu"] for w in workers} != set(self.external_cpus) or sorted(
            s for w in workers for s in w["sources"]
        ) != list(range(5)):
            raise RuntimeError(f"unexpected relay worker CPU or private TAP assignment: {workers}")
        identities = []
        for worker in workers:
            tid = worker["tid"]
            task = Path(f"/proc/{self.pid}/task/{tid}")
            if (
                not task.is_dir()
                or task.stat().st_uid != os.getuid()
                or os.sched_getaffinity(tid) != {worker["cpu"]}
            ):
                raise RuntimeError(f"unexpected actual worker identity or placement: {worker}")
            identities.append({k: worker[k] for k in ("tid", "role", "cpu", "sources")})
        config["verified_workers"] = identities
        return topology


def route_proof(fixture):
    p = argparse.ArgumentParser()
    p.add_argument("--wire-proof-only", action="store_true")
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--app-cpu", type=int, default=10)
    p.add_argument("--manager-cpu", type=int, default=15)
    p.add_argument("--router-cpus", default="2,5,8,14")
    p.add_argument("--client-cpus", default="0,1;3,4;6,7;11,12,13")
    p.add_argument("--delay-us", type=int, default=75)
    p.add_argument(
        "--scratch", action="store_true", help="Use one spare relay CPU and the separate low-rate proof lock"
    )
    a = p.parse_args()
    a.router_cpus = [int(x) for x in a.router_cpus.split(",")]
    a.groups = a.client_cpus.split(";")
    if (
        len(a.groups) != 4
        or len(set(a.router_cpus)) not in ((1,) if a.scratch else (4,))
        or a.app_cpu in a.router_cpus
    ):
        p.error(
            "use four unique external CPUs for the normal proof, one spare CPU for a scratch proof, and four client groups"
        )
    temp = Path(tempfile.gettempdir())
    lock = (
        Path(
            os.environ.get(
                "TWELVE_WIRE_PROOF_LOCK", str(temp / f"twelve-wire-proof-{os.getuid()}-cpu{a.app_cpu}.lock")
            )
        )
        if a.scratch
        else temp / f"twelve-router-{os.getuid()}.lock"
    )
    mutex = os.open(lock, os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    fcntl.flock(mutex, fcntl.LOCK_EX)
    os.sched_setaffinity(0, {a.manager_cpu})
    dest = a.output.resolve()
    dest.mkdir(parents=True, exist_ok=False)
    a.output, a.tag = dest.parent, dest.name
    a.server_ehash, a.aliases, a.users = 16384, 8, 16
    prefix = "tw" + uuid.uuid4().hex[:7]
    names = [prefix + v for v in ("s", "p", "c0", "c1", "c2", "c3")]
    created = []
    try:
        shape = fixture.set_up(a, names, created)
        base.write(dest / "shape.json", shape)
        base.write(dest / "wire-proof.json", fixture.sample(names[1]))
        print(json.dumps(shape[names[1]]["polling_relay"]), flush=True)
    finally:
        for name in reversed(created):
            base.priv("ip", "netns", "del", name, check=False)


if __name__ == "__main__":
    fixture = ShardedTap(Path(os.environ.get("TWELVE_SHARDED_TAP_RELAY", str(HERE / "relay"))))
    try:
        if "--wire-proof-only" in sys.argv:
            route_proof(fixture)
        else:
            base.main(
                network_setup=fixture.set_up,
                network_profile=PROFILE,
                network_sample=fixture.sample,
                delay_us=75,
            )
    finally:
        fixture.close()
