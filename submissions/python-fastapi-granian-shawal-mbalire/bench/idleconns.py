#!/usr/bin/env python3
"""Opens idle keep-alive connections in steps, for measuring the server's memory per connection.

Each connection sends one request (like a user's first GET /feed), reads the response, then stays
open and idle. After each step it prints one JSON line and waits for a line on stdin before the
next step, so bench/linux.py can sample the server's memory in between.

    python idleconns.py --steps 5000,10000,20000 --request feed
"""

import argparse
import asyncio
import json
import sys

REQUESTS = {
    "feed": b"GET /feed HTTP/1.1\r\nHost: x\r\n\r\n",
    "health": b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n",
    "none": b"",  # connect only: the cost of an accepted socket that never sent a byte
}


async def open_one(host: str, port: int, src: str, raw: bytes, held: list) -> bool:
    try:
        reader, writer = await asyncio.open_connection(host, port, local_addr=src and (src, 0))
        if raw:
            writer.write(raw)
            head = await reader.readuntil(b"\r\n\r\n")
            length = 0
            for line in head.split(b"\r\n")[1:]:
                name, _, value = line.partition(b":")
                if name.strip().lower() == b"content-length":
                    length = int(value)
            await reader.readexactly(length)
        held.append(writer)
        return True
    except (OSError, asyncio.IncompleteReadError, asyncio.LimitOverrunError):
        return False


async def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=3000)
    p.add_argument("--steps", default="5000,10000,20000,35000")
    p.add_argument("--request", choices=sorted(REQUESTS), default="feed")
    p.add_argument("--batch", type=int, default=500)
    args = p.parse_args()
    raw, held, failed = REQUESTS[args.request], [], 0
    loop = asyncio.get_running_loop()
    for target in (int(s) for s in args.steps.split(",")):
        while len(held) < target:
            n = min(args.batch, target - len(held))
            # 250 connections per source address (127.1.x.y) keeps clear of the ephemeral port range
            srcs = ((len(held) + i) // 250 for i in range(n))
            if not args.host.startswith("127."):
                srcs = (None for _ in range(n))  # one client IP across the network; its port range is the cap
            oks = await asyncio.gather(*(open_one(args.host, args.port, s if s is None else f"127.1.{s // 250}.{s % 250 + 1}",
                                                  raw, held) for s in srcs))
            failed += oks.count(False)
            if failed > target // 10 + 100:
                print(json.dumps({"error": "too many failed connections", "held": len(held), "failed": failed}),
                      flush=True)
                return
        print(json.dumps({"held": len(held), "failed": failed}), flush=True)
        await loop.run_in_executor(None, sys.stdin.readline)


if __name__ == "__main__":
    try:
        import uvloop
        uvloop.install()
    except ImportError:
        pass
    asyncio.run(main())
