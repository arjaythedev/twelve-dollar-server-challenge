"""Exercise real Linux TCP connections and locate XDP redirects on one private veth."""

import argparse
from contextlib import contextmanager, ExitStack
import errno
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from network import BASE_IP, HERE, PrivateLink, STACK_IP
from common import docker, tcp_socket, validate_cpus
from sources import materialize


def build_probe(work):
    source = HERE.parents[1] / "bypass"
    executable = work / "link-probe"
    with tempfile.TemporaryDirectory(dir=work) as directory:
        built = Path(directory) / "probe"
        subprocess.run(
            ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-DTWELVE_AF_XDP", "-static",
             str(HERE / "link_probe.c"), str(source / "link.c"), str(source / "link_xdp.c"), "-o", str(built)],
            check=True,
        )
        os.replace(built, executable)
    return executable


def logs(container):
    return subprocess.check_output(
        ["docker", "--host", "unix:///var/run/docker.sock", "logs", container],
        stderr=subprocess.STDOUT, text=True,
    )


@contextmanager
def receiver(link, args, name, mode, ip, port=3000, *, shared=False, measure=False,
             rx=0, tx=0, gso=False, failure=None):
    caps = ["--cap-add", "NET_RAW"]
    if mode != "packet":
        caps += ["--cap-add", "NET_ADMIN", "--cap-add", "BPF"]
    container = docker(
        "run", "-d", *link.base(name), "--network", "container:" + link.server_ids[0], *caps,
        "--cpuset-cpus", str(args.server_cpu), "--pids-limit", "8", "--memory", "64m",
        "--memory-swap", "64m", "--ulimit", "memlock=33554432:33554432",
        "--mount", f"type=bind,src={args.work/'link-probe'},dst=/probe-link,readonly",
        "--entrypoint", "/probe-link", link.image, str(link.host_inode), mode, ip, str(port),
        str(int(shared)), str(int(measure)), str(rx), str(tx), str(int(gso)),
    )
    try:
        deadline = time.monotonic() + 5
        while True:
            output = logs(container)
            running = docker("inspect", "-f", "{{.State.Running}}", container) == "true"
            if not running or "ready mode=" in output:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("private probe startup timed out: " + output)
            time.sleep(0.02)
        if failure:
            if running or failure not in output:
                raise AssertionError(output)
        elif not running or "caps=0 no_new_privs=1" not in output:
            raise RuntimeError(output)
        yield container
        if running:
            docker("stop", "-t", "2", container)
            if docker("inspect", "-f", "{{.State.ExitCode}}", container) != "0":
                raise RuntimeError(logs(container))
    finally:
        try:
            (args.out / (name + ".log")).write_text(logs(container))
        finally:
            docker("rm", "-f", container, check=False)


def peer_socket(link, client=True):
    provider = link.client_providers[0] if client else link.providers[0]
    return socket.socket(fileno=tcp_socket(provider))


def closed_port(link, ip):
    with peer_socket(link) as peer:
        peer.settimeout(2)
        result = peer.connect_ex((ip, 3000))
        if result != errno.ECONNREFUSED:
            raise OSError(result, "expected the link probe's closed-port RST")
        return peer.getsockopt(socket.SOL_SOCKET, socket.SO_INCOMING_CPU)


def sibling_http(link, listener):
    with peer_socket(link) as client:
        client.settimeout(2)
        client.connect((BASE_IP, 3100))
        client.sendall(b"GET /health HTTP/1.0\r\n\r\n")
        accepted, _ = listener.accept()
        with accepted:
            accepted.settimeout(2)
            assert accepted.recv(256).startswith(b"GET /health ")
            accepted.sendall(b"HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok")
        response = b""
        while block := client.recv(256):
            response += block
        assert response.startswith(b"HTTP/1.0 200 OK") and response.endswith(b"\r\n\r\nok")


def fields(line):
    return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line)}


def four_connections(args, link, name, mode, *, shared=False, materialized=False, gso=False):
    ip = BASE_IP if shared else STACK_IP
    measure = mode != "packet" and shared
    with ExitStack() as cleanup:
        listener = None
        if shared:
            listener = cleanup.enter_context(peer_socket(link, client=False))
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((BASE_IP, 3100))
            listener.listen(4)
            listener.settimeout(2)
            with receiver(link, args, name + "-occupied", mode, ip, 3100, shared=True,
                          failure="Address already in use"):
                pass
        with receiver(link, args, name, mode, ip, shared=shared, measure=measure, gso=gso) as app:
            if listener:
                sibling_http(link, listener)
                # Let the preceding kernel HTTP FIN/ACK settle before measuring
                # TCP counters specifically for the four redirected SYNs.
                time.sleep(0.02)
            before = link.server_counters()
            incoming = [closed_port(link, ip) for _ in range(2)]
            docker("kill", "--signal", "USR1", app)
            incoming.extend(closed_port(link, ip) for _ in range(2))
            after = link.server_counters()
            assert before["InSegs"] == after["InSegs"] and before["OutSegs"] == after["OutSegs"]
            if listener:
                sibling_http(link, listener)
            elif mode != "packet":
                with receiver(link, args, name + "-active", mode, "10.241.1.252", failure="busy"):
                    pass
        if listener:
            sibling_http(link, listener)
    out = (args.out / (name + ".log")).read_text()
    stats = fields(next(line for line in out.splitlines() if line.startswith("probe_syn=")))
    assert stats["probe_syn"] == 4
    if shared:
        assert stats["arp_requests"] == 0 and stats["arp_replies"] == 1
    if materialized or mode == "packet":
        assert stats["invalid_checksums"] == 0
    if measure:
        cpu = args.server_cpu if mode == "xdp-skb" else args.client_cpu
        assert re.findall(r"xdp redirect_cpus=([^ ]+) total=(\d+)", out)[-1] == (f"{cpu}:4", "4")
    return {"probe": stats, "linux_reply_incoming_cpu": incoming}


def burst(args, link, name, mode, rx, tx):
    with receiver(link, args, name, mode, STACK_IP, measure=mode != "packet", rx=rx, tx=tx) as app:
        closed_port(link, STACK_IP)
        docker("kill", "--signal", "STOP", app)
        try:
            with ExitStack() as active:
                for _ in range(2048):
                    peer = active.enter_context(peer_socket(link))
                    peer.setblocking(False)
                    assert peer.connect_ex((STACK_IP, 3000)) == errno.EINPROGRESS
                time.sleep(0.02)
        finally:
            docker("kill", "--signal", "CONT", app)
        time.sleep(0.25)
    out = (args.out / (name + ".log")).read_text()
    if mode == "packet":
        stats = fields(next(line for line in reversed(out.splitlines()) if line.startswith("packet link ")))
        total, dropped = stats["kernel_packets"], stats["kernel_drops"]
        received, sent = stats["rx"], stats["tx"]
    else:
        stats = fields(next(line for line in reversed(out.splitlines()) if line.startswith("xdp kernel ")))
        cpu, count, total = re.findall(r"xdp redirect_cpus=(\d+):(\d+) total=(\d+)", out)[-1]
        assert int(cpu) == args.server_cpu and count == total
        total, dropped = int(total), stats["rx_dropped"]
        frames = fields(next(line for line in reversed(out.splitlines()) if line.startswith("XDP link ")))
        received, sent = frames["rx"], frames["tx"]
    assert total >= 2049 and received == sent == total - dropped
    assert (dropped >= 1536 if rx == 512 else dropped == 0), stats
    return {"rx_slots": rx, "tx_slots": tx or 512, "received": received, "sent": sent,
            "kernel_packets_or_redirects": total, "kernel_drops": dropped, "stats": stats}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", required=True, type=Path, help="directory prepared with build-lab.sh")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--server-cpu", required=True, type=int)
    parser.add_argument("--client-cpu", required=True, type=int)
    parser.add_argument("--manager-cpu", required=True, type=int)
    parser.add_argument("--ring-burst", action="store_true", help="also queue 2,048 ordinary connections")
    parser.add_argument("--hybrid", action="store_true", help="also test XDP RX with AF_PACKET GSO TX")
    args = parser.parse_args()
    args.work = args.work.resolve()
    args.out = args.out.resolve()
    args.out.mkdir(exist_ok=False)
    if os.getuid() == 0:
        raise ValueError("Run the fixture as an ordinary user with local Docker access")
    validate_cpus(args.manager_cpu, args.server_cpu, [[args.client_cpu]])
    build_probe(args.work)
    tokens = HERE.parents[3] / "seed/tokens.json"
    results = {"kernel": os.uname().release, "server_cpu": args.server_cpu, "client_cpu": args.client_cpu}
    with tempfile.TemporaryDirectory(dir=args.work) as directory:
        with PrivateLink(Path(directory), args.out, args.server_cpu, [[args.client_cpu]], 2,
                         args.work, tokens, args.manager_cpu) as link:
            # These ordinary connect/send syscalls generate frames on this CPU.
            os.sched_setaffinity(0, {args.client_cpu})
            with receiver(link, args, "zero-copy", "xdp-zc", STACK_IP, failure="not supported"):
                pass
            with receiver(link, args, "secondary-alias", "xdp", "10.241.1.2", failure="assigned to Linux"):
                pass
            results["packet"] = four_connections(args, link, "packet", "packet")
            results["driver"] = four_connections(args, link, "driver", "xdp")
            results["shared_driver"] = four_connections(args, link, "shared-driver", "xdp", shared=True)
            materialize(link)
            results["shared_generic"] = four_connections(args, link, "shared-generic", "xdp-skb",
                                                          shared=True, materialized=True)
            if args.hybrid:
                results["hybrid"] = four_connections(args, link, "hybrid", "xdp-skb", shared=True,
                                                      materialized=True, gso=True)
            if args.ring_burst:
                results["bursts"] = []
                for mode, rx, tx in (("packet", 512, 0), ("packet", 8192, 0), ("packet", 16384, 0),
                                     ("xdp-skb", 512, 512), ("xdp-skb", 4096, 512), ("xdp-skb", 8192, 2048)):
                    results["bursts"].append(burst(args, link, f"{mode}-{rx}-{tx}", mode, rx, tx))
    (args.out / "link-check.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
