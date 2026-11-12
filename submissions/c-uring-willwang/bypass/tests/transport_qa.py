#!/usr/bin/env python3
"""Exercise one private HTTP server with ordinary Linux TCP sockets."""
import argparse
import base64
import fcntl
import hashlib
import hmac
import http.client
import json
import os
from pathlib import Path
import select
import signal
import socket
import struct
import termios
import time


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def wire(path, *, method="GET", body=b"", headers=()):
    lines = [f"{method} {path} HTTP/1.1", "Host: private.test", *headers]
    if body:
        lines.append(f"Content-Length: {len(body)}")
    return ("\r\n".join(lines) + "\r\n\r\n").encode("ascii") + body


def response(stream):
    line = stream.readline(1024)
    parts = line.split(None, 2)
    require(len(parts) >= 2 and parts[0] == b"HTTP/1.1", f"lost HTTP framing: {line!r}")
    headers = http.client.parse_headers(stream)
    sizes = headers.get_all("Content-Length", [])
    require(len(sizes) == 1 and sizes[0].isdigit(), f"invalid response length: {sizes!r}")
    size = int(sizes[0])
    require(size <= 8 * 1024 * 1024, f"unexpected response size: {size}")
    body = stream.read(size)
    require(len(body) == size, f"truncated response: {len(body)} of {size}")
    return int(parts[1]), body


def connect(args, small_window=False):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.settimeout(args.timeout)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if small_window:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_WINDOW_CLAMP, 4096)
        sock.connect((args.host, args.port))
        return sock
    except BaseException:
        sock.close()
        raise


def query(args, request):
    with connect(args) as sock, sock.makefile("rb") as stream:
        sock.sendall(request)
        return response(stream)


def refs(args):
    expected = {}
    for path, code in (("/health", 200), ("/feed", 200), ("/transport-qa-absent", 404)):
        got = query(args, wire(path))
        require(got[0] == code, f"{path} returned {got[0]}")
        expected[path] = got
    feed = json.loads(expected["/feed"][1])["posts"]
    require(bool(feed), "this regression needs at least one seeded post")
    point = f"/posts/{feed[0]['id']}"
    expected[point] = query(args, wire(point))
    require(expected[point][0] == 200 and json.loads(expected[point][1])["post"] == feed[0],
            "feed and independent post detail disagree")
    require(json.loads(expected["/health"][1]).get("db") == "ok", "SQLite health failed")
    return expected, point


def matches(expected, path, got):
    if path != "/health":
        return got == expected[path]
    status, body = got
    health = json.loads(body)
    saved_uptime = json.loads(expected[path][1])["uptime_s"]
    tick = health.get("uptime_s")
    valid = (status == 200 and health.get("status") == health.get("db") == "ok" and
             type(tick) is int and 0 <= saved_uptime <= tick)
    if valid:
        expected[path] = got
    return valid


def check_fragmentation(args, expected):
    body = b'{"fragment":"ordinary HTTP framing"}'
    header = wire("/transport-qa-absent", method="POST", body=body)[:-len(body)]
    with connect(args) as sock, sock.makefile("rb") as stream:
        for part in (header[:3], header[3:-1], header[-1:] + body[:5]):
            sock.sendall(part)
            require(not select.select([sock], [], [], 0.008)[0], "answered an incomplete HTTP request")
        sock.sendall(body[5:] + wire("/health"))
        require(response(stream) == expected["/transport-qa-absent"], "fragmented POST response")
        require(matches(expected, "/health", response(stream)), "request following split body")
    with connect(args) as sock:
        sock.sendall(b"GET /hea")
        sock.shutdown(socket.SHUT_WR)
        data = sock.recv(1024)
        require(not data or data.startswith(b"HTTP/1.1 400 "), f"partial EOF produced {data[:80]!r}")


def check_large_framing(args, expected):
    # The application limits decoded bodies to 16 KiB. A legal 12 KiB body
    # represented by one-byte chunks needs 72 KiB on the wire, so its parser
    # cannot finish until the stack returns multiple receive-window credits.
    content = b"x" * 16384
    requests = [wire("/transport-qa-absent", method="POST", body=content,
                     headers=("X-Padding: " + "a" * 12000,))]
    chunks = (b"POST /transport-qa-absent HTTP/1.1\r\nHost: private.test\r\n"
              b"Transfer-Encoding: chunked\r\n\r\n" + b"1\r\nx\r\n" * 12288 + b"0\r\n\r\n")
    requests.append(chunks)
    for request in requests:
        with connect(args) as sock, sock.makefile("rb") as stream:
            sock.sendall(request + wire("/health"))
            require(response(stream) == expected["/transport-qa-absent"], "large request framing")
            require(matches(expected, "/health", response(stream)), "request following a large body")
    return {"fixed_body": len(content), "chunked_body": 12288, "chunked_wire": len(chunks)}


def check_half_close(args, expected, point):
    # The first peer sends FIN before it reads any of the 137 responses.
    order = [("/feed", point, "/health", "/transport-qa-absent")[i % 4] for i in range(137)]
    with connect(args) as sock, sock.makefile("rb") as stream:
        sock.sendall(b"".join(wire(path) for path in order))
        sock.shutdown(socket.SHUT_WR)
        for i, path in enumerate(order):
            require(matches(expected, path, response(stream)), f"half-close response {i} for {path}")
        require(not stream.read(1), "extra bytes followed the final pipelined response")
    return len(order)


def unread(sock):
    return struct.unpack("=i", fcntl.ioctl(sock.fileno(), termios.FIONREAD, bytes(4)))[0]


def check_small_window(args, expected):
    repeats = 24
    expected_bytes = repeats * len(expected["/feed"][1])
    with connect(args, small_window=True) as slow, slow.makefile("rb") as stream:
        slow.setsockopt(socket.IPPROTO_TCP, socket.TCP_QUICKACK, 0)
        slow.sendall(wire("/feed") * repeats)
        until = time.monotonic() + args.timeout
        while not unread(slow) and time.monotonic() < until:
            time.sleep(0.002)
        time.sleep(0.12)
        queued = unread(slow)
        require(0 < queued < expected_bytes, f"receive buffering did not constrain the feed, got {queued}")
        before = time.monotonic()
        require(matches(expected, "/health", query(args, wire("/health"))), "slow client stalled another connection")
        other_ms = (time.monotonic() - before) * 1000
        slow.setsockopt(socket.IPPROTO_TCP, socket.TCP_QUICKACK, 1)
        for i in range(repeats):
            require(response(stream) == expected["/feed"], f"flow-controlled feed {i}")
    return {"initial_unread": queued, "expected_body_bytes": expected_bytes, "other_health_ms": round(other_ms, 2)}


def reset_peers(args, expected):
    # An abortive close leaves sent responses unread and also covers partial input.
    for _ in range(2):
        clients = []
        try:
            for i in range(12):
                sock = connect(args, small_window=True)
                clients.append(sock)
                sock.sendall(wire("/feed") * 7 if i % 2 else b"GET /feed HTTP/1.1\r\nHost: partial")
            time.sleep(0.025)
        finally:
            for sock in clients:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                sock.close()
        require(matches(expected, "/health", query(args, wire("/health"))), "server failed to recover after resets")


def snapshot(args):
    # The invoking private-link harness supplies its own foreground server PID.
    offset = args.server_log.stat().st_size
    os.kill(args.server_pid, signal.SIGUSR1)
    end = time.monotonic() + 1
    with args.server_log.open("rb") as log:
        log.seek(offset)
        partial = b""
        while time.monotonic() < end:
            lines = (partial + log.read()).split(b"\n")
            partial = lines.pop()
            for line in lines:
                if line.startswith(b"bypass stats "):
                    return {k: int(v) for k, v in
                            (part.split("=", 1) for part in line.decode().split()[2:])}
            time.sleep(0.005)
    raise AssertionError("the stack did not publish its resource counters")


def wait_clean(args):
    end = time.monotonic() + args.timeout
    while True:
        row = snapshot(args)
        if row["live"] == row["buffered"] == 0:
            return row
        if time.monotonic() >= end:
            raise AssertionError(f"resources retained after close/reset: {row}")
        time.sleep(0.02)


def check_admission_limit(args, expected):
    limit = args.admission_limit
    require(args.server_pid and 1 <= limit <= 64, "this check needs a dedicated stack capped at 1..64 connections")
    wait_clean(args)
    peers = []
    try:
        for _ in range(limit):
            peer = connect(args)
            peers.append(peer)
            peer.sendall(wire("/health"))
            with peer.makefile("rb") as stream:
                require(matches(expected, "/health", response(stream)), "connection below configured limit failed")
        require(snapshot(args)["live"] == limit, "live connection count disagrees with held peers")
        try:
            with connect(args) as extra:
                extra.sendall(wire("/health"))
                require(extra.recv(1) == b"", "application served a connection over the configured cap")
        except (ConnectionResetError, ConnectionRefusedError, BrokenPipeError):
            pass
        require(snapshot(args)["live"] == limit, "refused admission displaced an open client")
        peers.pop().close()
        deadline = time.monotonic() + args.timeout
        while snapshot(args)["live"] != limit - 1:
            require(time.monotonic() < deadline, "closed connection retained its admission slot")
            time.sleep(0.01)
        require(matches(expected, "/health", query(args, wire("/health"))), "a freed admission slot was not reusable")
    finally:
        for peer in peers:
            peer.close()
    stats = wait_clean(args)
    require(stats["peak"] == limit, "stack exceeded its advertised connection cap")
    return {"limit": limit, "peak_live": stats["peak"], "final_live": stats["live"]}


def tcp_retransmissions(sock):
    # Linux struct tcp_info has exposed this uint32 at byte 100 since 2.6.
    return struct.unpack_from("=I", sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 104), 100)[0]


def check_retransmission(args, expected):
    # Pausing this dedicated userspace TCP endpoint delays all acknowledgments.
    # Linux sends the real retransmission; the test never synthesizes a packet.
    require(args.server_pid and args.server_log, "retransmission needs the private bypass service PID and log")
    with connect(args) as sock, sock.makefile("rb") as stream:
        sock.sendall(wire("/health"))
        require(matches(expected, "/health", response(stream)), "loss probe did not first establish HTTP")
        before = snapshot(args)
        initial = tcp_retransmissions(sock)
        pidfd = os.pidfd_open(args.server_pid)
        try:
            signal.pidfd_send_signal(pidfd, signal.SIGSTOP)
            try:
                deadline = time.monotonic() + 0.5
                token = Path(f"/proc/{args.server_pid}/status")
                while not any(line.startswith("State:") and line.split()[1] == "T"
                              for line in token.read_text().splitlines()):
                    require(time.monotonic() < deadline, "private TCP endpoint did not pause")
                    time.sleep(0.002)
                sock.sendall(wire("/feed"))
                deadline = time.monotonic() + min(args.timeout, 3)
                while tcp_retransmissions(sock) == initial and time.monotonic() < deadline:
                    time.sleep(0.008)
                retries = tcp_retransmissions(sock) - initial
                require(retries > 0, "Linux TCP_INFO reported no retransmission while its peer was paused")
            finally:
                signal.pidfd_send_signal(pidfd, signal.SIGCONT)
        finally:
            os.close(pidfd)
        require(response(stream) == expected["/feed"], "retransmitted feed did not recover")
        sock.sendall(wire("/health"))
        require(matches(expected, "/health", response(stream)), "extra HTTP response after retransmission")
        after = snapshot(args)
        require(after["requests"] - before["requests"] == 2, "duplicate payload reached HTTP dispatch")
    return retries


def create_wire(args, marker):
    def encode(data):
        return base64.urlsafe_b64encode(data).rstrip(b"=")

    claims = json.dumps({"sub": "1", "username": "transport-qa", "exp": int(time.time()) + 300}).encode()
    signed = encode(b'{"alg":"HS256"}') + b"." + encode(claims)
    jwt = (signed + b"." + encode(hmac.digest(args.secret.encode(), signed, hashlib.sha256))).decode()
    return wire("/posts", method="POST", body=json.dumps({"body": marker}).encode(),
                headers=("Authorization: Bearer " + jwt, "Content-Type: application/json"))


def check_commit(args):
    gate = args.wal_gate
    require(not gate.exists(), f"WAL fault gate already exists: {gate}")
    suffix = f"{os.getpid()}-{time.monotonic_ns()}"
    durable, rejected = "qa-durable-" + suffix, "qa-rollback-" + suffix
    code, body = query(args, create_wire(args, durable))
    require(code == 201, f"durable control write returned {code}: {body[:128]}")
    accepted = json.loads(body)["post"]["id"]
    barrier = getattr(args, "wal_barrier", False)
    entered = gate.with_name(gate.name + ".entered")
    release = gate.with_name(gate.name + ".release")
    if barrier:
        require(not entered.exists() and not release.exists(), "WAL phase markers must start absent")
    gate.touch(exist_ok=False)
    try:
        if barrier:
            with connect(args) as sock, sock.makefile("rb") as stream:
                sock.sendall(create_wire(args, rejected))
                deadline = time.monotonic() + args.timeout
                while not entered.exists():
                    require(time.monotonic() < deadline, "SQLite never entered the tested WAL write")
                    require(not select.select([sock], [], [], 0.002)[0], "HTTP data arrived before WAL completion")
                require(not select.select([sock], [], [], 0.05)[0], "HTTP data arrived while the WAL write was suspended")
                release.touch(exist_ok=False)
                code, body = response(stream)
        else:
            code, body = query(args, create_wire(args, rejected))
        require(code == 500, f"failed WAL commit returned {code}: {body[:128]}")
    finally:
        gate.unlink()
        if barrier:
            entered.unlink(missing_ok=True)
            release.unlink(missing_ok=True)
    code, body = query(args, wire(f"/posts/{accepted}"))
    require(code == 200 and json.loads(body)["post"]["body"] == durable, "earlier ACK was lost")
    code, body = query(args, wire("/feed"))
    require(code == 200 and all(p["body"] != rejected for p in json.loads(body)["posts"]),
            "rolled-back row appeared in the feed")
    return {"acknowledged_id": accepted, "acknowledged_body": durable, "rejected_body": rejected}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--server-pid", type=int)
    parser.add_argument("--server-log", type=Path)
    parser.add_argument("--admission-limit", type=int, help="verify a dedicated stack configured with this small connection cap")
    parser.add_argument("--loss", action="store_true", help="briefly pause only this private userspace TCP endpoint until Linux retransmits")
    parser.add_argument("--wal-gate", type=Path, help="gate used by the server's WAL fault-injection preload")
    parser.add_argument("--wal-barrier", action="store_true", help="verify the peer receives zero bytes while the test preload holds the real WAL write")
    parser.add_argument("--secret", default=os.environ.get("JWT_SECRET", "twelve-dollar-challenge"))
    args = parser.parse_args()
    require(bool(args.server_pid) == bool(args.server_log), "provide both --server-pid and --server-log")
    require(not args.wal_barrier or args.wal_gate, "the WAL barrier requires a fault gate")
    result = {}
    expected, point = refs(args)
    check_fragmentation(args, expected)
    result["large_fixed_and_chunked_bytes"] = check_large_framing(args, expected)
    result["half_close_replies"] = check_half_close(args, expected, point)
    result["slow_reader"] = check_small_window(args, expected)
    reset_peers(args, expected)
    result["reset_connections"] = 24
    if args.loss:
        result["linux_client_retransmissions"] = check_retransmission(args, expected)
    if args.wal_gate:
        result["wal"] = check_commit(args)
    if args.admission_limit is not None:
        result["admission"] = check_admission_limit(args, expected)
    if args.server_pid:
        result["stack"] = wait_clean(args)
    print(json.dumps(result, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
