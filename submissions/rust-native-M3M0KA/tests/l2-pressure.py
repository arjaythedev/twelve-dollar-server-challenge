#!/usr/bin/env python3
"""Small L2 application burst/reuse correctness check; not a load benchmark."""
import asyncio
import json
import os

HOST = "127.0.0.1"
PORT = int(os.environ.get("L2_PRESSURE_PORT", "23309"))
CLIENTS = int(os.environ.get("L2_PRESSURE_CLIENTS", "384"))
WAVES = int(os.environ.get("L2_PRESSURE_WAVES", "3"))
BUFFER_COUNT = os.environ.get("L2_PRESSURE_BUFFER_COUNT", "256")
TIMEOUT = int(os.environ.get("L2_PRESSURE_TIMEOUT", "60"))
INTERLEAVE = os.environ.get("L2_PRESSURE_INTERLEAVE", "0") == "1"
ERROR_CHURN = os.environ.get("L2_PRESSURE_ERROR_CHURN", "0") == "1"

async def read_response(reader):
    raw = await reader.readuntil(b"\r\n\r\n")
    lines = raw.decode("latin1").split("\r\n")
    status = int(lines[0].split(" ", 2)[1])
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            key, value = line.split(":", 1)
            headers[key.lower()] = value.strip()
    size = int(headers["content-length"])
    body = await reader.readexactly(size)
    return status, headers, body

async def one_request(writer, path):
    writer.write((f"GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n").encode())

async def run_wave(wave):
    if INTERLEAVE:
        async def one_client():
            reader, writer = await asyncio.open_connection(HOST, PORT)
            try:
                await one_request(writer, "/feed")
                await writer.drain()
                await asyncio.sleep(0.25)
                status, headers, body = await read_response(reader)
                assert status == 200, status
                assert headers.get("content-type", "").startswith("application/json")
                assert len(json.loads(body).get("posts", [])) == 20
                await one_request(writer, "/health")
                await writer.drain()
                status, _, body = await read_response(reader)
                assert status == 200, status
                assert json.loads(body).get("status") == "ok"
            finally:
                writer.close()
                await writer.wait_closed()
        await asyncio.gather(*(one_client() for _ in range(CLIENTS)))
        print(f"wave={wave} concurrent={CLIENTS} interleaved=feed-health PASS", flush=True)
        await asyncio.sleep(0.25)
        return

    if ERROR_CHURN:
        peers = await asyncio.gather(*(
            asyncio.open_connection(HOST, PORT) for _ in range(CLIENTS)
        ))
        readers, writers = zip(*peers)
        for writer in writers:
            writer.write(b"GET / HTTP/1.0\r\nHost: localhost\r\n\r\n")
        await asyncio.gather(*(writer.drain() for writer in writers))
        failures = await asyncio.gather(*(read_response(reader) for reader in readers))
        assert all(status == 400 for status, _, _ in failures), [f[0] for f in failures]
        await asyncio.gather(*(reader.read() for reader in readers))
        for writer in writers:
            writer.close()
        await asyncio.gather(*(writer.wait_closed() for writer in writers), return_exceptions=True)
        await asyncio.sleep(0.25)
        print(f"wave={wave} invalid_requests={CLIENTS} status=400 closed=PASS", flush=True)

    peers = await asyncio.gather(*(
        asyncio.open_connection(HOST, PORT) for _ in range(CLIENTS)
    ))
    readers = [p[0] for p in peers]
    writers = [p[1] for p in peers]
    try:
        for writer in writers:
            await one_request(writer, "/feed")
        await asyncio.gather(*(w.drain() for w in writers))
        # Briefly leave all responses unread to exercise simultaneous receive,
        # ready-queue and send ownership. This is not a network performance test.
        await asyncio.sleep(0.25)
        feed = await asyncio.gather(*(read_response(r) for r in readers))
        for status, headers, body in feed:
            assert status == 200, status
            assert headers.get("content-type", "").startswith("application/json")
            payload = json.loads(body)
            assert len(payload.get("posts", [])) == 20
        for writer in writers:
            await one_request(writer, "/health")
        await asyncio.gather(*(w.drain() for w in writers))
        health = await asyncio.gather(*(read_response(r) for r in readers))
        for status, headers, body in health:
            assert status == 200, status
            assert json.loads(body).get("status") == "ok"
        print(f"wave={wave} concurrent={CLIENTS} feed=200 health=200 keepalive=PASS", flush=True)
    finally:
        for writer in writers:
            writer.close()
        await asyncio.gather(*(w.wait_closed() for w in writers), return_exceptions=True)
    await asyncio.sleep(0.25)

async def main():
    for wave in range(1, WAVES + 1):
        await asyncio.wait_for(run_wave(wave), TIMEOUT)
    if ERROR_CHURN:
        print("error_close_reuse=PASS subsequent_valid_feed_health=PASS", flush=True)
    print(f"result=PASS waves={WAVES} max_concurrency={CLIENTS} provided_buffers={BUFFER_COUNT}", flush=True)
    print("note=buffer-exhaustion stimulus; ENOBUFS occurrence was not directly instrumented", flush=True)

asyncio.run(main())
