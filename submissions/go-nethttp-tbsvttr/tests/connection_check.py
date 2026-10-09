#!/usr/bin/env python3
"""Linux loopback connection-pressure diagnostic using the bench/load.js user loop.

Run from the repository root against a fresh seed database. This is a Python
diagnostic, not an official k6 score. Multiple loopback source addresses avoid
mistaking the client's ephemeral-port range for a server connection limit.
"""
import argparse
import asyncio
from array import array
from collections import Counter
import json
import random
import resource
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("port", type=int)
parser.add_argument("--users", type=int, default=512)
parser.add_argument("--seconds", type=float, default=30)
parser.add_argument("--ramp", type=float, default=10)
parser.add_argument("--timeout", type=float, default=60)
parser.add_argument("--tokens", default="seed/tokens.json")
parser.add_argument("--seed", type=int, default=12)
parser.add_argument("--offset", type=int, default=0, help="distinct user/source-address range for another load-generator process")
args = parser.parse_args()
if args.users < 1 or min(args.seconds, args.timeout) <= 0 or args.ramp < 0:
    parser.error("users, seconds and timeout must be positive; ramp must be nonnegative")
with open(args.tokens) as source:
    tokens = json.load(source)

durations, elapsed_requests = array("d"), array("d")
errors = Counter()
stats = Counter()
successful_users = set()
started = time.monotonic()
hold_start, deadline = started + args.ramp, started + args.ramp + args.seconds


async def pause(seconds):
    await asyncio.sleep(max(0, min(seconds, deadline - time.monotonic())))


async def user(index):
    index += args.offset
    rng = random.Random(args.seed + index)
    token = tokens[index % len(tokens)]
    reader = writer = None
    alias = index // 20000
    address = f"127.0.{1 + alias // 250}.{1 + alias % 250}"

    async def disconnect():
        nonlocal reader, writer
        if writer is not None:
            writer.close()
            stats["connections"] -= 1
            reader = writer = None

    async def request(method, path, expected, body=None):
        nonlocal reader, writer
        begin = time.monotonic()
        measured = begin >= hold_start
        request_begin = None
        status, payload = 0, b""
        failure = None
        try:
            async with asyncio.timeout(args.timeout):
                if reader is not None and reader.at_eof():
                    await disconnect()
                if writer is None:
                    reader, writer = await asyncio.open_connection(
                        "127.0.0.1", args.port, local_addr=(address, 0), limit=65536)
                    stats["connections"] += 1
                    stats["peak_connections"] = max(stats["peak_connections"], stats["connections"])
                    stats["connects"] += 1
                headers = f"{method} {path} HTTP/1.1\r\nHost: localhost\r\n"
                data = b"" if body is None else json.dumps(body).encode()
                if method == "POST":
                    headers += (f"Authorization: Bearer {token['token']}\r\n"
                                f"Content-Type: application/json\r\nContent-Length: {len(data)}\r\n")
                request_begin = time.monotonic()
                writer.write(headers.encode() + b"\r\n" + data)
                await writer.drain()
                head = await reader.readuntil(b"\r\n\r\n")
                status = int(head.split(b"\r\n", 1)[0].split()[1])
                fields = dict(line.split(b":", 1) for line in head.lower().split(b"\r\n")[1:] if b":" in line)
                payload = await reader.readexactly(int(fields[b"content-length"]))
                if status not in expected:
                    failure = f"HTTP {status} {method} {path.split('/')[1]}"
                else:
                    successful_users.add(index)
                if fields.get(b"connection", b"").strip() == b"close":
                    stats["retired_connections"] += 1
                    await disconnect()
        except (OSError, TimeoutError, asyncio.IncompleteReadError, ValueError, KeyError) as error:
            failure = type(error).__name__
            status, payload = 0, b""
            await disconnect()
        if measured:
            end = time.monotonic()
            durations.append(0 if request_begin is None else (end - request_begin) * 1000)
            elapsed_requests.append((end - begin) * 1000)
            stats["requests"] += 1
            if failure:
                errors[failure] += 1
        return status, payload

    try:
        await pause(args.ramp * (index - args.offset) / args.users + rng.uniform(0, 5))
        while time.monotonic() < deadline:
            status, payload = await request("GET", "/feed", {200})
            if status != 200:
                await pause(rng.uniform(3, 7))
                continue
            posts = json.loads(payload)["posts"]
            post = rng.choice(posts)["id"]
            # The next request needs only the selected ID, not a retained feed per user.
            posts, payload = None, b""
            await pause(rng.uniform(3, 7))
            if time.monotonic() >= deadline:
                break
            await request("GET", f"/posts/{post}", {200})
            await pause(rng.uniform(3, 8))
            if time.monotonic() >= deadline:
                break
            if rng.random() < 0.15:
                await request("POST", f"/posts/{post}/like", {200, 201})
            if time.monotonic() < deadline and rng.random() < 0.02:
                await request("POST", "/posts", {201}, {"body": f"{token['username']} connection check {index} {time.time_ns()}"})
            await pause(rng.uniform(5, 15))
    finally:
        await disconnect()


async def main():
    loop_delay = 0

    async def progress():
        nonlocal loop_delay
        last_report = started
        while time.monotonic() < deadline:
            expected = time.monotonic() + 1
            await asyncio.sleep(1)
            now = time.monotonic()
            if now >= hold_start:
                loop_delay = max(loop_delay, (now - expected) * 1000)
            if now - last_report >= 30:
                print(json.dumps({"progress_seconds": round(now - started), **stats,
                                  "errors": sum(errors.values()), "event_loop_delay_ms": round(loop_delay, 1)}), flush=True)
                last_report = now

    reporting = asyncio.create_task(progress())
    try:
        await asyncio.gather(*(user(index) for index in range(args.users)))
    finally:
        reporting.cancel()
        await asyncio.gather(reporting, return_exceptions=True)
    ordered = sorted(durations)
    end_to_end = sorted(elapsed_requests)
    percentile = lambda samples, q: samples[min(len(samples) - 1, int((len(samples) - 1) * q))] if samples else None
    result = {"diagnostic": "Python mixed user loop; not an official k6 score",
              "configuration": vars(args), **stats, "successful_users": len(successful_users),
              "errors": dict(errors), "error_rate": sum(errors.values()) / max(1, len(ordered)),
              "p50_ms": percentile(ordered, .5), "p95_ms": percentile(ordered, .95), "p99_ms": percentile(ordered, .99),
              "end_to_end_p99_ms": percentile(end_to_end, .99), "event_loop_delay_ms": loop_delay,
              "elapsed_seconds": time.monotonic() - started,
              "load_generator_cpu_seconds": sum(resource.getrusage(resource.RUSAGE_SELF)[:2]),
              "load_generator_peak_rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss}
    result["pass"] = (bool(ordered) and result["successful_users"] == args.users
                      and result["error_rate"] < .01 and result["p95_ms"] < 500
                      and result["p99_ms"] < 1000 and loop_delay < 1000)
    print(json.dumps(result), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
