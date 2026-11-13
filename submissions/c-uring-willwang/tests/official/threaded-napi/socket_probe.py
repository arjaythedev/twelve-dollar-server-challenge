"""Measure the actual receive CPU and NAPI ID on the established application socket."""

import json
import os
import socket
import sys
import time

side, mode, *options = sys.argv[1:]
address = options[0] if options else "10.232.80.1"
port = 30471
count = int(options[1]) if len(options) > 1 else 16
if side == "server":
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((address, port))
        listener.listen(1)
        print("READY", flush=True)
        conn, _ = listener.accept()
        with conn:
            conn.settimeout(3)
            seen = []
            while len(seen) < count:
                if conn.recv(1) != b"x":
                    raise RuntimeError("unexpected test request")
                seen.append((conn.getsockopt(socket.SOL_SOCKET, 49), conn.getsockopt(socket.SOL_SOCKET, 56)))
                conn.sendall(b"y")
            print(
                json.dumps(
                    {
                        "mode": mode,
                        "receiver_affinity": sorted(os.sched_getaffinity(0)),
                        "observed_cpu_and_napi_id": seen,
                    }
                ),
                flush=True,
            )
else:
    with socket.create_connection((address, port), timeout=3) as conn:
        for _ in range(count):
            conn.sendall(b"x")
            if conn.recv(1) != b"y":
                raise RuntimeError("unexpected test reply")
            time.sleep(0.012)
