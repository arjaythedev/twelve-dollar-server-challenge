import argparse
import fcntl
import json
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

here = Path(__file__).resolve().parent
sys.path.insert(0, str(here.parent / "router"))
import run as h

napi = here.parent / "threaded-napi" / "private_napi.py"
p = argparse.ArgumentParser()
p.add_argument("--out", type=Path, required=True)
p.add_argument("--app-cpu", type=int, default=9)
p.add_argument("--remote-cpu", type=int, default=15)
p.add_argument("--count", type=int, default=128)
a = p.parse_args()
a.out = a.out.resolve()
a.out.mkdir(parents=True, exist_ok=False)
os.sched_setaffinity(0, {a.app_cpu})
lock = Path(
    os.environ.get(
        "TWELVE_WIRE_PROOF_LOCK",
        str(Path(tempfile.gettempdir()) / f"twelve-wire-proof-{os.getuid()}-cpu{a.app_cpu}.lock"),
    )
)
gate = lock.open("a")
fcntl.flock(gate, fcntl.LOCK_EX)


def proc(name, cpu, side):
    return [
        "sudo",
        "-n",
        "ip",
        "netns",
        "exec",
        name,
        "taskset",
        "-c",
        str(cpu),
        "setpriv",
        f"--reuid={os.getuid()}",
        f"--regid={os.getgid()}",
        "--init-groups",
        "--inh-caps=-all",
        "--ambient-caps=-all",
        "--bounding-set=-all",
        "--no-new-privs",
        "/usr/bin/python3",
        str(here / "check_flow.py"),
        side,
        "10.232.80.1",
        str(a.count),
    ]


def checksum_stats(ns):
    data = h.ns(ns, "nstat", "-asz")
    return {
        f[0]: int(f[1])
        for line in data.splitlines()
        if (f := line.split())
        and f[0] in ("TcpInCsumErrors", "TcpInErrs", "IpInHdrErrors", "IpExtInCsumErrors", "TcpRetransSegs")
    }


def measure_flow(mode, server, client, server_dev, details):
    out = a.out / mode
    out.mkdir()
    app = subprocess.Popen(
        proc(server, a.app_cpu, "server"), text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    perf = None
    perflog = None
    try:
        if not select.select([app.stdout], [], [], 4)[0]:
            raise RuntimeError("flow server startup timed out")
        line = app.stdout.readline().strip()
        pid = int(line.split()[1])
        if not line.startswith("READY "):
            raise RuntimeError(f"flow server failed: {line}")
        ctrl = out / "perf-control"
        ack = out / "perf-ack"
        os.mkfifo(ctrl)
        os.mkfifo(ack)
        cfd = os.open(ctrl, os.O_RDWR | os.O_NONBLOCK)
        afd = os.open(ack, os.O_RDWR | os.O_NONBLOCK)
        perflog = (out / "perf.log").open("w")
        (napi_pid,) = [r["pid"] for r in details["napi"]]
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
                f"fifo:{ctrl},{ack}",
                "-g",
                "-e",
                "ipi:ipi_send_cpu",
                "-e",
                "napi:napi_poll",
                "-p",
                f"{pid},{napi_pid}",
                "-o",
                str(out / "perf.data"),
            ],
            stdout=perflog,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        os.write(cfd, b"enable\n")
        if not select.select([afd], [], [], 7)[0] or b"ack" not in os.read(afd, 128):
            raise RuntimeError(f"task-specific IPI trace did not arm: {(out/'perf.log').read_text()}")
        result = subprocess.run(
            proc(client, a.remote_cpu, "client"), text=True, capture_output=True, timeout=15
        )
        if result.returncode:
            raise RuntimeError(result.stderr)
        remote = json.loads(result.stdout)
        stdout, stderr = app.communicate(timeout=5)
        if app.returncode:
            raise RuntimeError(stderr)
        service = json.loads(stdout)
        if perf.poll() is None:
            os.write(cfd, b"stop\n")
        perf.wait(timeout=6)
        os.close(cfd)
        os.close(afd)
        raw = h.priv("perf", "script", "-i", out / "perf.data")
        (out / "ipi-stacks.txt").write_text(raw + "\n")

        def from_task(line, target):
            return re.search(rf"\s{target}\s+\[\d+\]", line) is not None

        blocks = [
            block
            for block in raw.split("\n\n")
            if "ipi_send_cpu:" in block and from_task(block.splitlines()[0], pid)
        ]
        polls = [
            header
            for block in raw.split("\n\n")
            if block.strip()
            and (header := block.splitlines()[0])
            and ":napi_poll:" in header
            and from_task(header, napi_pid)
        ]
        activities = []
        for header in polls:
            match = re.search(r"\[(\d+)\].*for device (\S+) work (\d+) budget (\d+)", header)
            if match:
                activities.append((int(match[1]), match[2], int(match[3])))
        if (
            not activities
            or any(cpu != a.app_cpu or dev != server_dev for cpu, dev, work in activities)
            or sum(work for _, _, work in activities) < a.count
        ):
            raise RuntimeError(
                f"private NAPI trace did not see expected work on the service CPU: {activities[:3]}, total {len(activities)}"
            )
        tx = [
            b
            for b in blocks
            if f"cpu={a.remote_cpu} " in b
            and "__tcp_transmit_skb+" in b
            and ("net_rps_send_ipi+" in b or re.search(r"\b(?:veth_xmit|tun_net_xmit)\b", b))
        ]
        callbacks = {}
        for b in tx:
            line = next(s for s in b.splitlines() if "ipi_send_cpu:" in s)
            match = re.search(r"cpu=(\d+) callsite=(\S+) callback=(\S+)", line)
            key = " ".join(match.groups()) if match else line.strip()
            callbacks[key] = callbacks.get(key, 0) + 1
        if service["cpus"] != [a.app_cpu] or not set(service["napi_ids"]) <= {
            n["id"] for n in details["napi"]
        }:
            raise RuntimeError(f"incorrect private RX placement: {service}")
        return {
            "mode": mode,
            "service": service,
            "client": remote,
            "all_task_ipi": len(blocks),
            "device_tx_stack_ipi": len(tx),
            "device_tx_callbacks": callbacks,
            "napi_trace": {
                "polls": len(activities),
                "work": sum(row[2] for row in activities),
                "cpus": sorted({r[0] for r in activities}),
                "devices": sorted({r[1] for r in activities}),
            },
            "sample_event": "ipi:ipi_send_cpu",
            "app_cpu": a.app_cpu,
            "remote_cpu": a.remote_cpu,
            "napi": details,
        }
    finally:
        if app.poll() is None:
            app.terminate()
        if perf and perf.poll() is None:
            perf.terminate()
        if perflog:
            perflog.close()


def trial(mode):
    suffix = uuid.uuid4().hex[:7]
    server = "ts" + suffix
    client = "tc" + suffix
    nslist = []
    relay = None
    relay_pid = None
    relay_log = None
    try:
        for n in (server, client):
            h.priv("ip", "netns", "add", n)
            nslist.append(n)
            h.ip(n, "link", "set", "lo", "up")
        h.ns(client, "sysctl", "-q", "-w", f"net.core.rps_default_mask={1<<a.remote_cpu:x}")
        if mode == "veth":
            sd, cd = "svc", "peer"
            h.ip(
                server,
                "link",
                "add",
                sd,
                "numrxqueues",
                "1",
                "numtxqueues",
                "1",
                "type",
                "veth",
                "peer",
                "name",
                cd,
                "numrxqueues",
                "1",
                "numtxqueues",
                "1",
                "netns",
                client,
            )
        else:
            sd, cd = "t" + suffix, "labtap"
            relay_log = (a.out / "relay.log").open("w")
            relay = subprocess.Popen(
                [
                    "sudo",
                    "-n",
                    "taskset",
                    "-c",
                    str(a.remote_cpu),
                    os.environ.get("TWELVE_TAP_RELAY", str(here / "relay")),
                    str(os.getuid()),
                    str(os.getgid()),
                    "0",
                    str(a.out / "relay-stats.json"),
                    f"/run/netns/{server}",
                    f"/run/netns/{client}",
                ],
                text=True,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=relay_log,
            )
            if not select.select([relay.stdout], [], [], 5)[0]:
                raise RuntimeError("TAPs did not appear")
            ready = relay.stdout.readline().strip()
            if not ready.startswith("READY "):
                raise RuntimeError(f"relay failed: {(a.out/'relay.log').read_text()}")
            relay_pid = int(ready.split()[1])
            h.ip(server, "link", "set", "labtap", "name", sd)
        for i, (n, dev) in enumerate(((server, sd), (client, cd)), 1):
            h.ip(n, "link", "set", dev, "address", f"02:12:0d:00:00:{i:02x}")
            h.ip(n, "addr", "add", f"10.232.80.{i}/30", "dev", dev)
            h.ip(n, "link", "set", dev, "mtu", "1500", "up")
            h.ns(n, "ethtool", "-K", dev, "gro", "on", "gso", "on")
        if mode == "veth":
            h.ns(client, "ethtool", "-K", cd, "tso", "off")
        rec = json.loads(
            h.ns(
                server,
                "/usr/bin/python3",
                napi if mode == "veth" else here / "tap_napi.py",
                sd,
                a.app_cpu,
                os.stat("/run/netns/" + server).st_ino,
                os.stat("/proc/self/ns/net").st_ino,
            )
        )
        if relay:
            relay.stdin.write("G")
            relay.stdin.flush()
        mtu_ok = h.ns(
            client,
            "taskset",
            "-c",
            str(a.remote_cpu),
            "ping",
            "-n",
            "-M",
            "do",
            "-s",
            "1472",
            "-c",
            "2",
            "-i",
            ".02",
            "-W",
            "1",
            "10.232.80.1",
        )
        too_big = subprocess.run(
            [
                "sudo",
                "-n",
                "ip",
                "netns",
                "exec",
                client,
                "taskset",
                "-c",
                str(a.remote_cpu),
                "ping",
                "-n",
                "-M",
                "do",
                "-s",
                "1473",
                "-c",
                "1",
                "-W",
                "1",
                "10.232.80.1",
            ],
            capture_output=True,
            text=True,
        )
        if too_big.returncode == 0:
            raise RuntimeError("over-MTU DF packet passed")
        data = measure_flow(mode, server, client, sd, rec)
        data.update(
            mtu1500=mtu_ok,
            oversize_df=too_big.stdout + too_big.stderr,
            server_stats=checksum_stats(server),
            client_stats=checksum_stats(client),
        )
        return data
    finally:
        if relay_pid:
            os.kill(relay_pid, signal.SIGTERM)
        if relay:
            try:
                relay.wait(timeout=3)
            except subprocess.TimeoutExpired:
                relay.terminate()
        if relay_log:
            relay_log.close()
        for name in reversed(nslist):
            h.priv("ip", "netns", "del", name, check=False)


results = []
for mode in ("veth", "tap"):
    results.append(trial(mode))
    h.write(a.out / "proof.json", results)
print(
    json.dumps(
        [
            {
                k: r[k]
                for k in (
                    "mode",
                    "all_task_ipi",
                    "device_tx_stack_ipi",
                    "device_tx_callbacks",
                    "service",
                    "client",
                )
            }
            for r in results
        ]
    )
)
