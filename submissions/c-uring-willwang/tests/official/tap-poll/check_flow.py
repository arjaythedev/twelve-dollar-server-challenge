"""Exchange deterministic binary payloads and read the service socket's real TCP path metadata."""

import argparse
import hashlib
import json
import os
import socket
import struct

p = argparse.ArgumentParser()
p.add_argument("side", choices=("server", "client"))
p.add_argument("host")
p.add_argument("count", type=int)
a = p.parse_args()
chunk = bytes(range(251)) * 40
sizes = (32, 128, 1100, 4096, 9000)


def receive(conn, n):
    out = bytearray()
    while len(out) < n:
        part = conn.recv(n - len(out))
        if not part:
            raise OSError("short private test response")
        out += part
    return bytes(out)


if a.side == "server":
    with socket.socket() as l:
        l.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        l.bind((a.host, 34591))
        l.listen(2)
        print(f"READY {os.getpid()}", flush=True)
        c, _ = l.accept()
        with c:
            c.settimeout(6)
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            cpus = set()
            ids = set()
            for i in range(a.count):
                req = receive(c, 4)
                if struct.unpack("!I", req)[0] != i:
                    raise ValueError("wrong request sequence")
                cpus.add(c.getsockopt(socket.SOL_SOCKET, 49))
                ids.add(c.getsockopt(socket.SOL_SOCKET, 56))
                c.sendall(chunk[: sizes[i % len(sizes)]])
            print(
                json.dumps(
                    {
                        "count": a.count,
                        "cpus": sorted(cpus),
                        "napi_ids": sorted(ids),
                        "mss": c.getsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG),
                    }
                ),
                flush=True,
            )
else:
    with socket.create_connection((a.host, 34591), timeout=6) as c:
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        dig = hashlib.sha256()
        used = 0
        for i in range(a.count):
            c.sendall(struct.pack("!I", i))
            n = sizes[i % len(sizes)]
            data = receive(c, n)
            if data != chunk[:n]:
                raise ValueError("payload mismatch")
            dig.update(data)
            used += n
        print(
            json.dumps(
                {
                    "count": a.count,
                    "bytes": used,
                    "sha256": dig.hexdigest(),
                    "mss": c.getsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG),
                }
            )
        )
