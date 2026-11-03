#!/usr/bin/env python3
"""Low-memory diagnostic of the challenge user model. NOT the official k6 test."""
import argparse
import array
import asyncio
from collections import Counter
import datetime as dt
import json
import random
import resource
import socket
import time
from pathlib import Path


def percentile(values, fraction):
    if not values:
        return None
    position = (len(values) - 1) * fraction
    low = int(position)
    high = min(low + 1, len(values) - 1)
    return values[low] + (values[high] - values[low]) * (position - low)


async def main(args):
    Path(args.output).mkdir(parents=True, exist_ok=True)
    tokens = json.loads(Path(args.tokens).read_text())
    address = socket.gethostbyname(args.host)
    local_ips = args.local_ips.split(",") if args.local_ips else []
    random.seed(712367 + args.offset)
    start = asyncio.get_running_loop().time() + args.start_at - time.time()
    hold_start, hold_end = start + args.ramp, start + args.ramp + args.hold
    samples = array.array("d")
    request_samples = array.array("d")
    counts, errors, endpoints = Counter(), Counter(), Counter()
    lag = []
    active = connected = peak_active = peak_connected = 0
    cpu_start = time.process_time()

    async def user(index):
        nonlocal active, connected, peak_active, peak_connected
        active += 1
        peak_active = max(active, peak_active)
        reader = writer = None
        token = tokens[(args.offset + index) % len(tokens)]
        auth = ("Authorization: Bearer " + token["token"] + "\r\n").encode()

        def disconnect():
            nonlocal reader, writer, connected
            if writer is not None:
                writer.close()
                connected -= 1
            reader = writer = None

        async def request(method, path, body=None, authenticated=False):
            nonlocal reader, writer, connected, peak_connected
            began = asyncio.get_running_loop().time()
            request_began = began
            status = 0
            content = b""
            try:
                async with asyncio.timeout(30):
                    if writer is None:
                        local = (local_ips[index % len(local_ips)], 0) if local_ips else None
                        reader, writer = await asyncio.open_connection(address, args.port, local_addr=local)
                        connected += 1
                        peak_connected = max(peak_connected, connected)
                    request_began = asyncio.get_running_loop().time()
                    payload = body or b""
                    header = (f"{method} {path} HTTP/1.1\r\nHost: {args.host}\r\n"
                              "Connection: keep-alive\r\n").encode()
                    if authenticated:
                        header += auth + b"Content-Type: application/json\r\n"
                    if method == "POST":
                        header += f"Content-Length: {len(payload)}\r\n".encode()
                    writer.write(header + b"\r\n" + payload)
                    await writer.drain()
                    lines = (await reader.readuntil(b"\r\n\r\n")).split(b"\r\n")
                    status = int(lines[0].split()[1])
                    headers = dict(line.lower().split(b":", 1) for line in lines[1:] if b":" in line)
                    length = int(headers[b"content-length"].strip())
                    if not 0 <= length <= 2 * 1024 * 1024:
                        raise ValueError("invalid response length")
                    content = await reader.readexactly(length)
                    if headers.get(b"connection", b"").strip() == b"close":
                        disconnect()
            except (OSError, TimeoutError, ValueError, KeyError, asyncio.IncompleteReadError,
                    asyncio.LimitOverrunError) as exc:
                status = 0
                errors[type(exc).__name__] += 1
                disconnect()
            ended = asyncio.get_running_loop().time()
            counts["requests"] += 1
            failed = status == 0 or status >= 400
            counts["failed"] += failed
            expected = (200, 201) if path.endswith("/like") else (201,) if method == "POST" else (200,)
            counts["check_failures"] += status not in expected
            if hold_start <= ended < hold_end:
                samples.append((ended - began) * 1000)
                request_samples.append((ended - request_began) * 1000)
                counts["hold_failed"] += failed
                counts["hold_check_failures"] += status not in expected
                endpoints["feed" if path == "/feed" else "create" if path == "/posts"
                          else "like" if path.endswith("/like") else "post"] += 1
            return status, content

        try:
            await asyncio.sleep(random.uniform(0, 5))
            # Spread the ramp-down over 30 seconds, then allow the current
            # iteration to finish, up to the shared ten-second grace period.
            retire = hold_end + 30 * (1 - index / args.vus)
            iteration = -1
            while asyncio.get_running_loop().time() < retire:
                iteration += 1
                status, body = await request("GET", "/feed")
                if status != 200:
                    await asyncio.sleep(random.uniform(3, 7))
                    continue
                try:
                    post_id = random.choice(json.loads(body)["posts"])["id"]
                except (ValueError, KeyError, IndexError, TypeError):
                    counts["semantic_failures"] += 1
                    await asyncio.sleep(random.uniform(3, 7))
                    continue
                # Do not retain an entire feed during the think periods.
                del body
                await asyncio.sleep(random.uniform(3, 7))
                await request("GET", f"/posts/{post_id}")
                await asyncio.sleep(random.uniform(3, 8))
                if random.random() < .15:
                    await request("POST", f"/posts/{post_id}/like", authenticated=True)
                if random.random() < .02:
                    stamp = dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
                    payload = json.dumps({"body": f"{token['username']} says hi at {stamp} "
                                                  f"(VU {args.offset + index + 1}, iter {iteration})"},
                                         ensure_ascii=False, separators=(",", ":")).encode()
                    await request("POST", "/posts", payload, True)
                await asyncio.sleep(random.uniform(5, 15))
        finally:
            disconnect()
            active -= 1

    async def heartbeat():
        deadline = start
        while deadline < hold_end + 40:
            await asyncio.sleep(max(0, deadline - asyncio.get_running_loop().time()))
            now = asyncio.get_running_loop().time()
            if hold_start <= deadline < hold_end:
                lag.append(max(0, now - deadline))
                counts["minimum_active_hold"] = min(counts.get("minimum_active_hold", args.vus), active)
            if int(deadline - start) % 10 == 0:
                Path(args.output, "progress.json").write_text(json.dumps(dict(
                    elapsed_seconds=now - start, active=active, connected=connected,
                    requests=counts["requests"], failed=counts["failed"],
                    hold_requests=len(samples), scheduling_lag_seconds=max(0, now - deadline))))
            deadline += 1

    beat = asyncio.create_task(heartbeat())
    tasks = []
    for index in range(args.vus):
        if index % 100 == 0:
            await asyncio.sleep(max(0, start + args.ramp * index / args.vus -
                                    asyncio.get_running_loop().time()))
        tasks.append(asyncio.create_task(user(index)))
    await asyncio.sleep(max(0, hold_end + 40 - asyncio.get_running_loop().time()))
    for task in tasks:
        if not task.done():
            task.cancel()
    outcomes = await asyncio.gather(*tasks, return_exceptions=True)
    unexpected = [repr(x) for x in outcomes if isinstance(x, BaseException)
                  and not isinstance(x, asyncio.CancelledError)]
    await beat
    ordered = sorted(samples)
    ordered_requests = sorted(request_samples)
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    with (output / "hold-latencies.f64").open("wb") as stream:
        samples.tofile(stream)
    with (output / "hold-request-latencies.f64").open("wb") as stream:
        request_samples.tofile(stream)
    report = dict(diagnostic_only=True, vus=args.vus, offset=args.offset,
                  start_at=args.start_at, ramp_seconds=args.ramp, hold_seconds=args.hold,
                  peak_active=peak_active, peak_connected=peak_connected,
                  counts=dict(counts), errors=dict(errors), hold_endpoints=dict(endpoints),
                  hold=dict(requests=len(samples), rps=len(samples) / args.hold,
                            failure_rate=counts["hold_failed"] / max(1, len(samples)),
                            p95_ms=percentile(ordered, .95), p99_ms=percentile(ordered, .99),
                            request_p95_ms=percentile(ordered_requests, .95),
                            request_p99_ms=percentile(ordered_requests, .99)),
                  client=dict(cpu_seconds=time.process_time() - cpu_start,
                              max_rss_kib=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
                              scheduling_lag_p95_seconds=percentile(sorted(lag), .95),
                              scheduling_lag_max_seconds=max(lag, default=0)),
                  unexpected_errors=unexpected[:10],
                  limitations="Python diagnostic, not k6. Latency includes connection time. "
                              "No automatic HTTP retries; completed requests define hold samples.")
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report), flush=True)
    return 1 if unexpected or counts["semantic_failures"] else 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="server")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--tokens", default="/seed/tokens.json")
    parser.add_argument("--vus", type=int, required=True)
    parser.add_argument("--offset", type=int, default=0)
    parser.add_argument("--local-ips", default="")
    parser.add_argument("--start-at", type=float, required=True)
    parser.add_argument("--ramp", type=float, default=60)
    parser.add_argument("--hold", type=float, default=120)
    parser.add_argument("--output", required=True)
    raise SystemExit(asyncio.run(main(parser.parse_args())))
