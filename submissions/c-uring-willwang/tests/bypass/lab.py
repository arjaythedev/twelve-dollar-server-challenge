"""Isolated one-link AF_PACKET versus Linux TCP experiments."""

import argparse
from contextlib import contextmanager
import errno
import hashlib
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from network import BASE_IP, CLIENT_IP, HERE, PrivateLink, STACK_IP, receive
from common import group_of, tcp_socket, validate_cpus

REPO = HERE.parents[3]
PORT = 19432


def save(path, data):
    staged = path.with_name(f".{path.name}.pending")
    try:
        staged.write_text(json.dumps(data, indent=2) + "\n")
        staged.replace(path)
    finally:
        staged.unlink(missing_ok=True)


def digest(path):
    with Path(path).open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def client_socket(link, host, timeout=3):
    s = socket.socket(fileno=tcp_socket(link.client_providers[0]))
    try:
        s.settimeout(timeout)
        s.connect((host, PORT))
        return s
    except BaseException:
        s.close()
        raise


def health(link, host):
    c = http.client.HTTPConnection(host, PORT, timeout=1)
    try:
        c.sock = client_socket(link, host, 1)
        c.request("GET", "/health", headers={"Connection": "close"})
        response = c.getresponse()
        return response.status == 200 and json.loads(response.read()).get("db") == "ok"
    except (OSError, ValueError, http.client.HTTPException):
        return False
    finally:
        c.close()


def probe(args, link):
    with link.broker("packet") as (address, _):
        ctl, fd, meta = receive(address)
        with ctl, socket.socket(fileno=fd) as raw:
            if raw.family != socket.AF_PACKET or raw.type != socket.SOCK_RAW:
                raise ValueError("invalid private packet descriptor")
            result = {
                "link": meta,
                "client": CLIENT_IP,
                "linux_service": BASE_IP,
                "stack_service": STACK_IP,
                "family": raw.family.name,
                "type": raw.type.name,
                "manager_uid": os.geteuid(),
            }
            if args.echo:
                cmd = [
                    "taskset",
                    "-c",
                    str(args.server_cpu),
                    str(args.echo),
                    "--interface",
                    "bench0",
                    "--address",
                    STACK_IP,
                    "--mode",
                    "packet",
                    "--packet-fd",
                    str(fd),
                    "--port",
                    str(PORT),
                ]
                before = link.server_counters()
                with (args.out / "echo.log").open("w") as log:
                    proc = subprocess.Popen(cmd, pass_fds=(fd,), stdout=log, stderr=log)
                    try:
                        deadline = time.monotonic() + 4
                        while proc.poll() is None and time.monotonic() < deadline:
                            try:
                                with client_socket(link, STACK_IP, 0.3):
                                    raise AssertionError("the link probe unexpectedly accepted TCP")
                            except OSError as exc:
                                if exc.errno == errno.ECONNREFUSED:
                                    break
                        else:
                            raise RuntimeError("no userspace RST: " + (args.out / "echo.log").read_text())
                    finally:
                        if proc.poll() is None:
                            proc.terminate()
                        proc.wait(timeout=3)
                after = link.server_counters()
                result["tcp_before"] = before
                result["tcp_after"] = after
                if any(after[k] != before[k] for k in ("CurrEstab", "InSegs", "OutSegs")):
                    raise ValueError("kernel TCP handled a stack-owned packet")
                result["private_peer_connect_result"] = "ECONNREFUSED"
                result["echo_log"] = (args.out / "echo.log").read_text()
            save(args.out / "probe.json", result)
            print(json.dumps(result), flush=True)


@contextmanager
def application(args, link, db, name, secret, source=None):
    if args.mode == "xdp":
        from xdp_service import application as xdp_application

        with xdp_application(args, link, db, name, secret, source) as service:
            yield service
        return
    out = args.out / name
    out.mkdir()
    with link.broker(args.mode) as (address, broker_group):
        info = {
            "binary": str(args.binary),
            "mode": args.mode,
            "broker": str(address),
            "cpu": args.server_cpu,
            "port": PORT,
            "secret": secret,
            "db": str(db),
            "network": str(link.root),
            "runtime": str(out / "runtime.json"),
            "preload": str(args.work / "veth-bin/allocate.so"),
            "packet_rx_frames": getattr(args, "packet_rx_frames", None),
            "packet_tx_frames": getattr(args, "packet_tx_frames", None),
            "packet_gso": getattr(args, "packet_gso", False),
        }
        if source:
            info.update(source=str(source), seed_sha256=digest(source))
        with tempfile.NamedTemporaryFile(dir=link.root, mode="w") as cfg:
            json.dump(info, cfg)
            cfg.flush()
            unit = "twelve-bypass-" + uuid.uuid4().hex + ".scope"
            env = dict(os.environ)
            env.setdefault("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")
            cmd = [
                "systemd-run",
                "--user",
                "--scope",
                "--quiet",
                "--unit",
                unit,
                "-p",
                f"MemoryMax={args.memory_mib}M",
                "-p",
                "MemorySwapMax=0",
                "taskset",
                "-c",
                str(args.server_cpu),
                sys.executable,
                str(HERE / "launch.py"),
                cfg.name,
            ]
            pidfd = None
            with (out / "server.log").open("w") as log:
                service = subprocess.Popen(cmd, stdout=log, stderr=log, env=env)
                try:
                    deadline = time.monotonic() + 15
                    target = STACK_IP if args.mode == "packet" else BASE_IP
                    while service.poll() is None and time.monotonic() < deadline:
                        if (out / "runtime.json").exists():
                            if pidfd is None:
                                runtime = json.loads((out / "runtime.json").read_text())
                                pid = runtime["pid"]
                                pidfd = os.pidfd_open(pid)
                            if health(link, target):
                                break
                        time.sleep(0.02)
                    else:
                        raise RuntimeError(
                            "server health failed: " + (out / "server.log").read_text()[-3000:]
                        )
                    scope = group_of(pid)
                    if (
                        int((scope / "memory.max").read_text()) != args.memory_mib * 2**20
                        or int((scope / "memory.swap.max").read_text())
                        or os.sched_getaffinity(pid) != {args.server_cpu}
                    ):
                        raise ValueError("scored affinity or memory limit differs")
                    yield target, pid, scope, broker_group, out
                finally:
                    if pidfd is not None:
                        try:
                            if args.action == "score":
                                signal.pidfd_send_signal(pidfd, signal.SIGTERM)
                                try:
                                    service.wait(timeout=3)
                                except subprocess.TimeoutExpired:
                                    pass
                            signal.pidfd_send_signal(pidfd, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        os.close(pidfd)
                    elif service.poll() is None:
                        service.terminate()
                    try:
                        service.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        service.kill()
                        service.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("probe", "golden", "differential", "score"))
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--tokens", type=Path, default=REPO / "seed/tokens.json")
    parser.add_argument("--seed", type=Path, default=REPO / "seed/feed.db")
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--mode", choices=("baseline", "packet", "xdp"), default="packet")
    parser.add_argument(
        "--xdp-attachment",
        choices=("auto", "skb"),
        default="auto",
        help="force generic/SKB XDP after private veth RPS to keep receive work on the server core",
    )
    parser.add_argument(
        "--rx-cpu-stats",
        action="store_true",
        help="count XDP redirects by the actual CPU on which the program executed",
    )
    parser.add_argument(
        "--packet-rx-frames",
        type=int,
        help="set the private AF_PACKET receive ring capacity; the default uses the binary's capacity",
    )
    parser.add_argument(
        "--packet-tx-frames",
        type=int,
        help="set the AF_PACKET transmit ring capacity for the packet or hybrid XDP/GSO transport",
    )
    parser.add_argument("--xdp-rx-frames", type=int,
                        help="AF_XDP receive descriptor and FILL capacity in the server's measured UMEM")
    parser.add_argument("--xdp-tx-frames", type=int,
                        help="AF_XDP transmit/completion capacity in the server's measured UMEM")
    parser.add_argument("--packet-gso", action="store_true",
                        help="enable the server's validated userspace TCP frame aggregation on its AF_PACKET sender")
    parser.add_argument(
        "--shared-ip", action="store_true", help="exercise XDP on the kernel's veth IPv4 address"
    )
    parser.add_argument(
        "--wire-checksums",
        action="store_true",
        help="make the private Linux client emit complete on-wire TCP checksums for matched XDP comparisons",
    )
    parser.add_argument(
        "--native-bridge",
        action="store_true",
        help="pre-steer Ethernet frames through a private Linux bridge on the measured server CPU",
    )
    parser.add_argument("--echo", type=Path, help="optional userspace ARP/TCP-reject probe executable")
    parser.add_argument("--server-cpu", type=int, default=10)
    parser.add_argument("--client-cpus", default="3,4,5")
    parser.add_argument("--manager-cpu", type=int, default=15)
    parser.add_argument("--memory-mib", type=int, default=1730)
    parser.add_argument("--users", type=int, default=1000)
    parser.add_argument(
        "--shards",
        type=int,
        default=0,
        help="genuine Linux source IP/Go processes; default one per 12,500 users",
    )
    parser.add_argument("--ramp", type=int, default=10)
    parser.add_argument("--hold", type=int, default=20)
    parser.add_argument(
        "--close-timeout",
        type=int,
        default=120,
        help="observe the original PID while the private Linux peers complete FIN retransmissions",
    )
    parser.add_argument("--think", type=float, default=1)
    args = parser.parse_args()
    args.requested_memory_mib = args.memory_mib
    if args.native_bridge:
        # Both endpoint helpers have 8 MiB. Linux and AF_PACKET also use a 12 MiB broker.
        args.memory_mib = min(args.memory_mib, 1750 - 16 - (0 if args.mode == "xdp" else 12))
        if args.mode == "xdp" and args.action == "score":
            args.rx_cpu_stats = True
    if args.shared_ip and args.mode != "xdp":
        parser.error("--shared-ip requires --mode xdp")
    args.work = args.work.resolve()
    args.out = args.out.resolve()
    args.tokens = args.tokens.resolve()
    args.seed = args.seed.resolve()
    if args.binary:
        args.binary = args.binary.resolve()
    if args.action != "probe" and not args.binary:
        parser.error("--binary is required to run an application")
    if args.close_timeout < 1:
        parser.error("--close-timeout must be positive")
    if args.mode != "xdp" and (args.xdp_attachment != "auto" or args.rx_cpu_stats):
        parser.error("--xdp-attachment and --rx-cpu-stats require --mode xdp")
    if args.packet_rx_frames is not None and (args.mode != "packet" or args.packet_rx_frames <= 0):
        parser.error("--packet-rx-frames requires --mode packet and a positive frame count")
    if args.xdp_rx_frames is not None and (args.mode != "xdp" or args.xdp_rx_frames <= 0):
        parser.error("--xdp-rx-frames requires --mode xdp and a positive frame count")
    if args.xdp_tx_frames is not None and (args.mode != "xdp" or args.xdp_tx_frames <= 0):
        parser.error("--xdp-tx-frames requires --mode xdp and a positive frame count")
    if args.packet_gso and args.mode == "baseline":
        parser.error("--packet-gso selects an experimental userspace transport")
    packet_sender = args.mode == "packet" or (args.mode == "xdp" and args.packet_gso)
    if args.packet_tx_frames is not None and (args.packet_tx_frames <= 0 or not packet_sender):
        parser.error(
            "--packet-tx-frames requires a positive count and --mode packet or --mode xdp --packet-gso"
        )
    if args.out.exists():
        parser.error("choose a fresh output directory")
    args.out.mkdir(mode=0o700, parents=True)
    group = [int(n) for n in args.client_cpus.split(",")]
    if args.manager_cpu in group:
        parser.error("keep the telemetry CPU outside the client and server CPU sets")
    sibling_file = Path(f"/sys/devices/system/cpu/cpu{args.server_cpu}/topology/thread_siblings_list")
    if sibling_file.exists():
        siblings = set()
        for part in sibling_file.read_text().strip().split(","):
            values = list(map(int, part.split("-")))
            siblings.update(range(values[0], values[-1] + 1))
        if siblings.intersection([args.manager_cpu, *group]):
            parser.error("keep the server's SMT siblings free of client and observer work")
    validate_cpus(args.manager_cpu, args.server_cpu, [group])
    with tempfile.TemporaryDirectory(prefix="twelve-bypass-") as tmp:
        with PrivateLink(
            Path(tmp), args.out, args.server_cpu, [group], 1, args.work, args.tokens, args.manager_cpu
        ) as link:
            if args.native_bridge:
                from native_bridge import prepare

                prepare(link)
            if args.wire_checksums:
                from sources import materialize

                materialize(link)
            if args.action == "probe":
                probe(args, link)
            elif args.action == "score":
                from workload import score

                score(args, link)
            else:
                from correctness import check

                check(args, link)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    main()
