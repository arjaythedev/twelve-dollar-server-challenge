"""Provision genuine IPv4 source tuples for unmodified clients on one private veth."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import errno
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from network import BASE_IP, CLIENT_IP, HERE, PrivateLink, receive
from common import docker, tcp_socket, validate_cpus


def _run_setup(link, name, operation):
    if len(link.client_ids) != 1 or len(link.server_ids) != 1:
        raise ValueError("this operation requires exactly one private veth")
    helper = link.root / "sources"
    if not helper.exists():
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-static",
                        str(HERE / "sources.c"), "-o", str(helper)], check=True)
    return docker("run", "--rm", *link.base(name), "--network", "container:" + link.client_ids[0],
                  "--cap-add", "NET_ADMIN", "--pids-limit", "8", "--cpuset-cpus", str(link.manager_cpu),
                  "--memory", "12m", "--memory-swap", "12m",
                  "--mount", f"type=bind,src={helper},dst=/sources,readonly",
                  "--entrypoint", "/sources", link.image, str(link.host_inode), str(operation))


def provision(link, shard_count):
    """Return the UID/address for each scorer. Its ordinary Linux socket selects that source."""
    if not 1 <= shard_count <= 64 or len(link.client_ids) != 1 or len(link.server_ids) != 1:
        raise ValueError("provision 1–64 sources on exactly one private veth")
    existing = getattr(link, "source_routes", None)
    if existing is not None:
        if len(existing) != shard_count:
            raise ValueError("a different source allocation already exists on this veth")
        return existing
    output = _run_setup(link, "sources", shard_count)
    routes = []
    reserved = set()
    low = high = None
    for line in output.splitlines():
        kind, _, payload = line.partition(" ")
        if kind == "source":
            user, address = payload.split()
            routes.append({"uid": int(user), "address": address})
        elif kind == "port_range":
            low, high = map(int, payload.split())
        elif kind == "reserved" and payload:
            for portion in payload.split(","):
                first, _, last = portion.partition("-")
                reserved.update(range(int(first), int(last or first) + 1))
    if (len(routes) != shard_count or len({r["uid"] for r in routes}) != shard_count
            or len({r["address"] for r in routes}) != shard_count or low is None or not 0 < low <= high <= 65535):
        raise RuntimeError("invalid private source-route preflight: " + output)
    available = sum(port not in reserved for port in range(low, high + 1))
    link.source_limits = {"ephemeral_low": low, "ephemeral_high": high,
                          "available_per_address": available,
                          "total_available": available * len(routes)}
    link.source_routes = routes
    return routes


def materialize(link):
    """Make Linux finish IPv4 TCP checksums on the sending client veth before XDP reads the frame."""
    if hasattr(link, "wire_checksums"):
        return link.wire_checksums
    lines = _run_setup(link, "wire-checksums", "wire-checksums").splitlines()
    names = {}
    snapshots = {"before": [], "after": []}
    status = None
    fields = ("available", "requested", "active", "never_changed")
    for line in lines:
        kind, *values = line.split()
        if kind == "feature":
            names[int(values[0])] = values[1]
        elif kind in snapshots:
            if int(values[0]) != len(snapshots[kind]) or len(values) != 5:
                raise RuntimeError("incomplete private veth feature snapshot")
            snapshots[kind].append(dict(zip(fields, values[1:])))
        elif kind == "set_status":
            status = int(values[0])

    def mask(phase, field):
        return sum(int(block[field], 16) << (i * 32) for i, block in enumerate(snapshots[phase]))

    def bits(value):
        return [names.get(i, f"bit-{i}") for i in range(value.bit_length()) if value & (1 << i)]

    target = "tx-checksum-ip-generic"
    before = snapshots["before"]
    if not before or len(before) != len(snapshots["after"]) or target not in names.values():
        raise RuntimeError("missing private veth feature masks")
    changed = {k: bits(mask("before", k) ^ mask("after", k)) for k in fields}
    result = {"interface": "bench0", "driver": "veth", "endpoint": "client", "set_status": status,
              "requested_off": target, "labels_by_bit": names, **snapshots, "changed": changed}
    (link.output / "client-wire-checksums.json").write_text(json.dumps(result, indent=2) + "\n")
    if (status != 0 or changed["available"] or changed["never_changed"] or
            any(name != target for name in changed["requested"]) or
            target in bits(mask("after", "requested")) or
            {target, "tx-checksum-ipv4"} & set(bits(mask("after", "active")))):
        raise RuntimeError("private client TX checksum materialization failed; inspect client-wire-checksums.json")
    link.wire_checksums = result
    return result


def check(link, count):
    """Compare each ordinary client's getsockname() to accept() on the separate server."""
    routes = provision(link, count)
    peers = []
    with socket.socket(fileno=tcp_socket(link.providers[0])) as listener:
        listener.bind((BASE_IP, 0))
        listener.listen(count)
        listener.settimeout(15)
        port = listener.getsockname()[1]
        def accept_peers():
            for _ in routes:
                conn, peer = listener.accept()
                with conn:
                    conn.settimeout(3)
                    if conn.recv(1) != b"?":
                        raise ValueError("invalid private source probe")
                    peers.append(peer)
                    conn.sendall(b"!")

        # Only the disposable client's own output directory is shared with its container.
        with tempfile.TemporaryDirectory(dir=link.reports[0]) as temp:
            location = Path(temp)
            location.chmod(0o755)
            shutil.copy2(link.root / "sources", location / "identify")
            command = f"/out/{location.name}/identify"
            def dial(route):
                uid = route["uid"]
                response = docker("exec", "--user", f"{uid}:{uid}", link.client_ids[0],
                                  command, str(link.host_inode), "identify", str(port))
                address, source_port = response.split()
                if address != route["address"]:
                    raise ValueError(f"UID {uid} got {address}, expected {route['address']}")
                return address, int(source_port)
            with ThreadPoolExecutor(max_workers=1) as server, ThreadPoolExecutor(max_workers=6) as clients:
                collecting = server.submit(accept_peers)
                local = list(clients.map(dial, routes))
                collecting.result(timeout=20)
    if len(set(local)) != count or sorted(peers) != sorted(local):
        raise ValueError(f"local TCP tuples {local} differ from accepted peers {peers}")
    return {"routes": routes, "limits": link.source_limits, "client_getsockname": local,
            "server_accept_peers": peers, "server_ip": BASE_IP, "server_port": port}


def check_wire(link):
    """Measure actual TCP SYN bytes received by the private peer before and after ethtool."""
    port = 19486
    def checksum(data):
        padded = data + b"\0" * (len(data) % 2)
        total = sum(value for (value,) in struct.iter_unpack("!H", padded))
        while total >> 16:
            total = (total & 0xffff) + (total >> 16)
        return ~total & 0xffff

    def syn(raw):
        with socket.socket(fileno=tcp_socket(link.client_providers[0])) as client:
            client.settimeout(1)
            if client.connect_ex((BASE_IP, port)) != errno.ECONNREFUSED:
                raise RuntimeError("expected the private peer to reject the unbound TCP port")
        while True:
            frame, ancillary, _, _ = raw.recvmsg(2048, socket.CMSG_SPACE(20))
            if len(frame) < 54 or frame[12:14] != b"\x08\x00":
                continue
            ip = frame[14:]
            if ip[9] != 6 or ip[12:20] != socket.inet_aton(CLIENT_IP) + socket.inet_aton(BASE_IP):
                continue
            tcp = ip[4 * (ip[0] & 15):int.from_bytes(ip[2:4], "big")]
            if len(tcp) < 20 or tcp[13] & 0x12 != 0x02 or int.from_bytes(tcp[2:4], "big") != port:
                continue
            pseudo = ip[12:20] + b"\x00\x06" + struct.pack("!H", len(tcp))
            actual = int.from_bytes(tcp[16:18], "big")
            expected = checksum(pseudo + tcp[:16] + b"\0\0" + tcp[18:])
            status = next((struct.unpack_from("=I", value)[0] for level, typ, value in ancillary
                           if level == 263 and typ == 8), None)
            return {"frame_hex": frame.hex(), "wire_checksum": actual, "computed_checksum": expected,
                    "checksum_valid": actual == expected, "packet_auxdata_status": status}

    with link.broker("packet") as (path, _):
        ctl, fd, _ = receive(path)
        with ctl, socket.socket(fileno=fd) as raw:
            # SOL_PACKET/PACKET_AUXDATA records checksum metadata separately from the packet bytes.
            raw.setsockopt(263, 8, 1)
            raw.settimeout(2)
            before = [syn(raw) for _ in range(2)]
            features = materialize(link)
            after = [syn(raw) for _ in range(2)]
    if not all(s["checksum_valid"] for s in after):
        raise RuntimeError("the sender did not materialize the private peer's observed TCP checksum")
    return {"before": before, "after": after, "features": features}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--tokens", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--sources", type=int, default=32)
    parser.add_argument("--verify-wire", action="store_true", help="check private TCP SYN bytes")
    parser.add_argument("--server-cpu", required=True, type=int)
    parser.add_argument("--client-cpu", required=True, type=int)
    parser.add_argument("--manager-cpu", required=True, type=int)
    args = parser.parse_args()
    validate_cpus(args.manager_cpu, args.server_cpu, [[args.client_cpu]])
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix="twelve-source-check-") as tmp:
        with PrivateLink(Path(tmp), out, args.server_cpu, [[args.client_cpu]], 1, args.work.resolve(),
                         args.tokens.resolve(), args.manager_cpu) as link:
            result = check_wire(link) if args.verify_wire else check(link, args.sources)
    name = "wire-syn.json" if args.verify_wire else "sources.json"
    (out / name).write_text(json.dumps(result, indent=2) + "\n")
    print(out / name, flush=True)


if __name__ == "__main__":
    main()
