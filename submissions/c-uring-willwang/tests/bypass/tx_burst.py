"""Measure delayed TCP ACKs on a bounded cohort of ordinary private Ethernet clients."""

import argparse
from contextlib import ExitStack
import http.client
import json
import os
from pathlib import Path
import resource
import signal
import socket
import statistics
import struct
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from correctness import ref
from lab import PrivateLink, REPO, application, client_socket, digest, save
from sources import materialize


def tcp_info(sock):
    # Linux UAPI struct tcp_info through tcpi_total_retrans, in host byte order.
    data = sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 104)
    return struct.unpack_from("=I", data, 24)[0], struct.unpack_from("=I", data, 100)[0]


def ring_counters(pid, log):
    position = log.stat().st_size
    os.kill(pid, signal.SIGUSR1)
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        with log.open() as stream:
            stream.seek(position)
            report = stream.read()
        if "xdp kernel rx_dropped=" in report:
            line = next(line for line in report.splitlines() if line.startswith("hybrid link "))
            return {k: int(v) for k, v in (field.split("=") for field in line.split()[2:])}
        time.sleep(.01)
    raise TimeoutError(f"no fresh packet counters from PID {pid}")


def partial_request(clients, fragment):
    retrans_before = sum(tcp_info(sock)[1] for sock in clients)
    started = time.monotonic()
    for sock in clients:
        sock.sendall(fragment)
    sent_ms = (time.monotonic() - started) * 1000
    pending = clients
    observed = []
    while pending:
        outstanding = []
        for sock in pending:
            unacked, _ = tcp_info(sock)
            if unacked:
                outstanding.append(sock)
            else:
                observed.append((time.monotonic() - started) * 1000)
        pending = outstanding
        if pending:
            if time.monotonic() - started >= 4:
                raise TimeoutError(f"{len(pending)} sockets still have unacknowledged request data")
            time.sleep(.002)
    observed.sort()
    return {
        "send_duration_ms": sent_ms,
        "observed_ack_p50_ms": statistics.median(observed),
        "observed_ack_p99_ms": observed[int(.99 * len(observed))],
        "observed_ack_max_ms": observed[-1],
        "peer_retransmissions": sum(tcp_info(sock)[1] for sock in clients) - retrans_before,
    }


def run_arm(args, link, arm):
    with tempfile.TemporaryDirectory(dir=link.root) as tmp:
        db = Path(tmp) / "feed.db"
        ref.seed(db, "short")
        with application(args, link, db, arm, "fixture") as (host, pid, _, _, out), ExitStack() as cleanup:
            clients = [cleanup.enter_context(client_socket(link, host, 3)) for _ in range(args.connections)]
            for sock in clients:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            time.sleep(.3)
            log = out / "server.log"
            before = ring_counters(pid, log)
            if before["packet_tx_slots"] != args.packet_tx_frames:
                raise RuntimeError("the running application's reported TX depth differs from the test")
            turns = [partial_request(clients, b"GET /health HTTP/1.1\r\nHost: bench\r\nX-Burst: one")]
            turns.extend(partial_request(clients, b"x") for _ in range(2))
            after = ring_counters(pid, log)
            for sock in clients:
                sock.sendall(b"\r\n\r\n")
            for sock in clients:
                with http.client.HTTPResponse(sock) as response:
                    response.begin()
                    if response.status != 200 or json.loads(response.read()).get("db") != "ok":
                        raise RuntimeError("the completed HTTP request failed its independent health check")
            return {
                "tx_slots": after["packet_tx_slots"],
                "tx_slot_bytes": after["tx_slot_bytes"],
                "peer_count": len(clients),
                "http_health_responses": len(clients),
                "tx_busy": after["tx_busy"] - before["tx_busy"],
                "turns": turns,
            }


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--binary", type=Path, required=True)
    cli.add_argument("--work", type=Path, required=True)
    cli.add_argument("--out", type=Path, required=True)
    cli.add_argument("--tokens", type=Path, default=REPO / "seed/tokens.json")
    cli.add_argument("--tx-frames", type=int, nargs="+", default=[512, 4096, 4096, 512])
    cli.add_argument("--connections", type=int, default=2048)
    cli.add_argument("--server-cpu", type=int, required=True)
    cli.add_argument("--client-cpu", type=int, required=True)
    cli.add_argument("--manager-cpu", type=int, required=True)
    args = cli.parse_args()
    if args.connections < 1 or len({args.server_cpu, args.client_cpu, args.manager_cpu}) != 3:
        cli.error("specify a positive cohort size and separate server, client and observer CPUs")
    if any(n < 16 or n > 8192 or n & (n - 1) for n in args.tx_frames):
        cli.error("choose power-of-two TX frame counts from 16 through 8192 at this link's MTU")
    args.out, args.binary, args.work, args.tokens = (p.resolve() for p in (args.out, args.binary, args.work, args.tokens))
    args.out.mkdir(mode=0o700, parents=True)
    args.mode, args.action, args.xdp_attachment = "xdp", "golden", "skb"
    args.memory_mib, args.xdp_rx_frames, args.xdp_tx_frames = 1730, 8192, None
    args.shared_ip = args.packet_gso = args.rx_cpu_stats = True
    needed = args.connections + 128
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft < needed:
        resource.setrlimit(resource.RLIMIT_NOFILE, (needed, hard))
    os.sched_setaffinity(0, {args.client_cpu})
    arms = []
    with tempfile.TemporaryDirectory() as tmp:
        with PrivateLink(Path(tmp), args.out, args.server_cpu, [[args.client_cpu]], 1,
                         args.work, args.tokens, args.manager_cpu) as link:
            materialize(link)
            for i, frames in enumerate(args.tx_frames):
                args.packet_tx_frames = frames
                result = run_arm(args, link, f"arm{i}")
                arms.append(result)
                save(args.out / "result.json", {"binary_sha256": digest(args.binary), "arms": arms})
                print(json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
