#!/usr/bin/env python3
"""A lightweight stand-in for ../../bench/load.js that scales to 100k+ users on a few cores.

k6 spends megabytes per user, so it cannot drive the user counts that decide the score from one
machine. This copies load.js's behaviour exactly (ramping-vus ramp, think times, request mix, one
keep-alive connection per user, reconnect after `Connection: close`) using asyncio + uvloop, with
users split across processes. Each process connects from its own loopback source address, so one
generator is not capped by the ~28k ephemeral ports of a single (source, destination) pair.

    python3 loadgen.py --vus 20000 --ramp 60 --duration 300 --procs 7 --tokens tokens.json

Prints a JSON summary: requests, error rate, p95/p99 of request duration (send to last body byte,
connect time excluded, like k6's http_req_duration), and errors grouped by kind.
"""

import argparse
import asyncio
import json
import multiprocessing as mp
import random
import sys
import time
from array import array

LIKE_PROB = 0.15
POST_PROB = 0.02
TIMEOUT_S = 60.0  # k6's default request timeout


class Conn:
    """One user's keep-alive connection: HTTP/1.1, Content-Length bodies only."""

    __slots__ = ("host", "port", "src", "reader", "writer")

    def __init__(self, host: str, port: int, src: str):
        self.host, self.port, self.src = host, port, src
        self.reader = self.writer = None

    def drop(self) -> None:
        if self.writer is not None:
            self.writer.transport.abort()
        self.reader = self.writer = None

    async def request(self, raw: bytes) -> tuple[int, bytes]:
        if self.writer is None:
            self.reader, self.writer = await asyncio.open_connection(self.host, self.port, local_addr=(self.src, 0))
        self.writer.write(raw)
        head = await self.reader.readuntil(b"\r\n\r\n")
        lines = head.split(b"\r\n")
        status = int(lines[0].split(b" ", 2)[1])
        length, close = 0, False
        for line in lines[1:]:
            name, _, value = line.partition(b":")
            name = name.strip().lower()
            if name == b"content-length":
                length = int(value)
            elif name == b"connection" and value.strip().lower() == b"close":
                close = True
        body = await self.reader.readexactly(length) if length else b""
        if close:
            self.drop()
        return status, body


class Stats:
    def __init__(self, counters, slot: int):
        self.latencies_ms = array("f")
        self.errors: dict[str, int] = {}
        self.counters, self.slot = counters, slot

    def ok(self, ms: float) -> None:
        self.latencies_ms.append(ms)
        self.counters[self.slot * 2] += 1

    def fail(self, kind: str) -> None:
        self.errors[kind] = self.errors.get(kind, 0) + 1
        self.counters[self.slot * 2] += 1
        self.counters[self.slot * 2 + 1] += 1


async def timed(conn: Conn, raw: bytes, stats: Stats, ok_statuses: tuple[int, ...]) -> bytes | None:
    try:
        if conn.writer is None:  # connect outside the timed span, like k6's http_req_connecting
            conn.reader, conn.writer = await asyncio.wait_for(
                asyncio.open_connection(conn.host, conn.port, local_addr=(conn.src, 0)), TIMEOUT_S)
        start = time.perf_counter()
        status, body = await asyncio.wait_for(conn.request(raw), TIMEOUT_S)
        elapsed_ms = (time.perf_counter() - start) * 1000
    except asyncio.TimeoutError:
        conn.drop()
        stats.fail("timeout")
        return None
    except (OSError, asyncio.IncompleteReadError, ValueError, IndexError) as exc:
        conn.drop()
        stats.fail(type(exc).__name__ + (f"[{exc.errno}]" if getattr(exc, "errno", None) else ""))
        return None
    if status >= 400:  # k6 counts 4xx/5xx in http_req_failed
        stats.fail(f"http {status}")
        return None
    stats.ok(elapsed_ms)
    return body if status in ok_statuses else None


async def user(n: int, start_at: float, stop_at: float, ctx: dict, stats: Stats) -> None:
    await asyncio.sleep(max(0.0, start_at - time.monotonic()))
    token_row = ctx["tokens"][(n - 1) % len(ctx["tokens"])]
    host_hdr = ctx["host_header"]
    auth = b"Authorization: Bearer " + token_row["token"].encode() + b"\r\nContent-Type: application/json\r\n"
    feed_req = b"GET /feed HTTP/1.1\r\nHost: " + host_hdr + b"\r\n\r\n"
    conn = Conn(ctx["host"], ctx["port"], ctx["src"])
    first = True
    while time.monotonic() < stop_at:
        if first:
            await asyncio.sleep(random.uniform(0, 5))
            first = False
        body = await timed(conn, feed_req, stats, (200,))
        if body is None:
            await asyncio.sleep(random.uniform(3, 7))
            continue
        posts = json.loads(body)["posts"]
        post_id = random.choice(posts)["id"]
        await asyncio.sleep(random.uniform(3, 7))
        await timed(conn, b"GET /posts/%d HTTP/1.1\r\nHost: %s\r\n\r\n" % (post_id, host_hdr), stats, (200,))
        await asyncio.sleep(random.uniform(3, 8))
        if random.random() < LIKE_PROB:
            await timed(conn, b"POST /posts/%d/like HTTP/1.1\r\nHost: %s\r\n%sContent-Length: 0\r\n\r\n"
                        % (post_id, host_hdr, auth), stats, (200, 201))
        if random.random() < POST_PROB:
            payload = json.dumps({"body": f"{token_row['username']} says hi at {time.time():.3f} (VU {n})"}).encode()
            await timed(conn, b"POST /posts HTTP/1.1\r\nHost: %s\r\n%sContent-Length: %d\r\n\r\n%s"
                        % (host_hdr, auth, len(payload), payload), stats, (201,))
        await asyncio.sleep(random.uniform(5, 15))
    conn.drop()


def worker(slot: int, args, t0: float, counters, results) -> None:
    try:
        import uvloop
        uvloop.install()
    except ImportError:
        pass
    with open(args.tokens) as f:
        tokens = json.load(f)
    ctx = {"tokens": tokens, "host": args.host, "port": args.port, "src": f"127.0.{slot // 250}.{slot % 250 + 2}",
           "host_header": f"{args.host}:{args.port}".encode()}
    if not args.host.startswith("127."):
        ctx["src"] = "0.0.0.0"
    stats = Stats(counters, slot)
    stop_at = t0 + args.ramp + args.duration

    async def run() -> None:
        # ramping-vus: user n (1-based) starts at ramp * (n - 1) / vus
        users = [user(n, t0 + args.ramp * (n - 1) / args.vus, stop_at, ctx, stats)
                 for n in range(slot + 1, args.vus + 1, args.procs)]
        await asyncio.gather(*users)

    asyncio.run(run())
    results.put((stats.latencies_ms.tobytes(), stats.errors))


def percentile(sorted_values, q: float) -> float:
    if not sorted_values:
        return 0.0
    return sorted_values[min(len(sorted_values) - 1, int(q * len(sorted_values)))]


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=3000)
    p.add_argument("--vus", type=int, required=True)
    p.add_argument("--ramp", type=float, default=60)
    p.add_argument("--duration", type=float, default=300)
    p.add_argument("--procs", type=int, default=mp.cpu_count())
    p.add_argument("--tokens", default="tokens.json")
    args = p.parse_args()

    counters = mp.Array("q", 2 * args.procs, lock=False)
    results = mp.Queue()
    t0 = time.monotonic() + 2  # every process starts the ramp from the same instant
    procs = [mp.Process(target=worker, args=(slot, args, t0, counters, results)) for slot in range(args.procs)]
    for proc in procs:
        proc.start()

    end = t0 + args.ramp + args.duration
    last = 0
    while any(proc.is_alive() for proc in procs) and results.qsize() < args.procs:
        time.sleep(10)
        reqs = sum(counters[i] for i in range(0, len(counters), 2))
        errs = sum(counters[i] for i in range(1, len(counters), 2))
        t = time.monotonic() - t0
        print(f"[{t:6.0f}s] {reqs - last:7d} req in 10s ({(reqs - last) / 10:7.0f}/s)  errors total {errs}",
              file=sys.stderr, flush=True)
        last = reqs
        if time.monotonic() > end + 120:
            break

    latencies, errors = array("f"), {}
    for _ in procs:
        raw, errs = results.get(timeout=180)
        chunk = array("f")
        chunk.frombytes(raw)
        latencies.extend(chunk)
        for kind, n in errs.items():
            errors[kind] = errors.get(kind, 0) + n
    for proc in procs:
        proc.join(timeout=10)

    ordered = sorted(latencies)
    failed = sum(errors.values())
    total = len(ordered) + failed
    summary = {
        "vus": args.vus, "requests": total, "rps": round(total / (args.ramp + args.duration), 1),
        "error_rate": round(failed / total, 5) if total else 1.0,
        "p95_ms": round(percentile(ordered, 0.95), 2), "p99_ms": round(percentile(ordered, 0.99), 2),
        "max_ms": round(ordered[-1], 2) if ordered else 0.0, "errors": errors,
    }
    summary["pass"] = summary["p95_ms"] < 500 and summary["p99_ms"] < 1000 and summary["error_rate"] < 0.01
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
