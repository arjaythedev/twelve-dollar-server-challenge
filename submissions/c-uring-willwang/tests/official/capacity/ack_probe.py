"""Count standalone Linux TCP ACKs on the first two requests of an HTTP connection."""

import argparse
from collections import Counter
import http.client
import socket
import statistics
import struct
import time


def pure_acks(sock):
    # Linux uapi struct tcp_info: tcpi_segs_in and tcpi_data_segs_in.
    info = sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 160)
    if len(info) < 156:
        raise RuntimeError("The kernel does not expose TCP_INFO segment counters")
    segments = struct.unpack_from("=I", info, 140)[0]
    data_segments = struct.unpack_from("=I", info, 152)[0]
    return segments - data_segments


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("port", type=int)
    parser.add_argument("--path", default="/feed")
    parser.add_argument("--connections", type=int, default=24)
    parser.add_argument("--split-ms", type=float, default=0,
                        help="Delay the last two bytes of each request")
    parser.add_argument("--nagle", action="store_true", help="Enable client-side Nagle")
    args = parser.parse_args()
    if args.connections < 1 or args.split_ms < 0:
        parser.error("Connections must be positive and split time must be nonnegative")
    request = f"GET {args.path} HTTP/1.1\r\nHost: local\r\n\r\n".encode("ascii")
    acks = [Counter(), Counter()]
    latency_ms = [[], []]
    for _ in range(args.connections):
        with socket.create_connection((args.host, args.port), timeout=5) as sock:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, int(not args.nagle))
            for ordinal in range(2):
                before = pure_acks(sock)
                start_ns = time.monotonic_ns()
                if args.split_ms:
                    sock.sendall(request[:-2])
                    time.sleep(args.split_ms / 1000)
                    sock.sendall(request[-2:])
                else:
                    sock.sendall(request)
                with http.client.HTTPResponse(sock) as response:
                    response.begin()
                    if response.status != 200 or not response.read():
                        raise RuntimeError(f"Unexpected response with status {response.status}")
                elapsed_ms = (time.monotonic_ns() - start_ns) / 1_000_000
                latency_ms[ordinal].append(elapsed_ms)
                acks[ordinal][pure_acks(sock) - before] += 1
    for ordinal in range(2):
        print(f"request={ordinal + 1} extra_pure_ACK_histogram={dict(acks[ordinal])} "
              f"median_ms={statistics.median(latency_ms[ordinal]):.3f}")


if __name__ == "__main__":
    main()
