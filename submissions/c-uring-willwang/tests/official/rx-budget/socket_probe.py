"""Small TCP placement probe used inside the fixture-owned server and client namespaces."""

import json
import os
from pathlib import Path
import socket
import sys
import time


PORT = 30473
SIZES = (1, 48, 1400, 4096)
INCOMING_CPU = getattr(socket, "SO_INCOMING_CPU", 49)


def exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError("probe peer closed early")
        data.extend(chunk)
    return data


def publish(path, value):
    temporary = path.with_suffix(".partial")
    temporary.write_text(json.dumps(value))
    temporary.replace(path)


def serve(folder, total):
    rows = []
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.settimeout(30)
        listener.bind(("0.0.0.0", PORT))
        listener.listen(16)
        publish(folder / "ready.json", {"pid": os.getpid(), "uid": os.getuid(),
                "affinity": sorted(os.sched_getaffinity(0)), "cgroup": Path("/proc/self/cgroup").read_text().strip()})
        for _ in range(total):
            sock, peer = listener.accept()
            with sock:
                sock.settimeout(3)
                results = []
                for index, size in enumerate(SIZES):
                    data = exact(sock, size)
                    cpu = sock.getsockopt(socket.SOL_SOCKET, INCOMING_CPU)
                    if data != bytes([index + 1]) * size:
                        raise ValueError("incorrect probe payload")
                    results.append({"bytes": size, "incoming_cpu": cpu})
                    sock.sendall(bytes([index + 1]))
                rows.append({"peer": peer[0], "local": sock.getsockname()[0], "samples": results})
    publish(folder / "received.json", rows)
    # Keep the cgroup available for a last counter snapshot.
    for _ in range(300):
        if (folder / "release").exists():
            break
        time.sleep(0.05)


def send(address, cpu):
    os.sched_setaffinity(0, {cpu})
    with socket.create_connection((address, PORT), timeout=3) as sock:
        for index, size in enumerate(SIZES):
            sock.sendall(bytes([index + 1]) * size)
            if exact(sock, 1) != bytes([index + 1]):
                raise ValueError("incorrect probe response")
            time.sleep(0.004)


if __name__ == "__main__":
    if sys.argv[1] == "server":
        serve(Path(sys.argv[2]), int(sys.argv[3]))
    else:
        send(sys.argv[2], int(sys.argv[3]))
