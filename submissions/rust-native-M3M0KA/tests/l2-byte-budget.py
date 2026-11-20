#!/usr/bin/env python3
"""Force test-only aggregate L2 byte limits, then verify normal recovery."""
import asyncio
import json
import os
import socket

HOST = "127.0.0.1"
PORT = int(os.environ.get("L2_PRESSURE_PORT", "23309"))

async def response(reader):
    head = await reader.readuntil(b"\r\n\r\n")
    lines = head.decode("latin1").split("\r\n")
    status = int(lines[0].split(" ", 2)[1])
    headers = {key.lower(): value.strip() for key, value in
               (line.split(":", 1) for line in lines[1:] if ":" in line)}
    size = int(headers["content-length"])
    body = await reader.readexactly(size)
    return status, body

async def input_pressure():
    peers = await asyncio.gather(*(asyncio.open_connection(HOST, PORT) for _ in range(6)))
    for _, writer in peers:
        writer.write(b"X" * 12000)
    await asyncio.gather(*(writer.drain() for _, writer in peers))
    await asyncio.sleep(0.5)
    for _, writer in peers:
        writer.close()
    await asyncio.gather(*(writer.wait_closed() for _, writer in peers), return_exceptions=True)
    await asyncio.sleep(2)
    print("input_pressure=6 partial 12KB requests drained")

async def output_pressure():
    peers = await asyncio.gather(*(asyncio.open_connection(HOST, PORT) for _ in range(16)))
    readers, writers = zip(*peers)
    for writer in writers:
        sock = writer.get_extra_info("socket")
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        writer.write(b"GET /feed HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
    await asyncio.gather(*(writer.drain() for writer in writers))
    await asyncio.sleep(0.25)
    results = await asyncio.gather(*(response(reader) for reader in readers), return_exceptions=True)
    successes = bad_request = internal_error = 0
    for result in results:
        if isinstance(result, BaseException):
            continue
        status, body = result
        if status == 200:
            assert len(json.loads(body).get("posts", [])) == 20
            successes += 1
        elif status == 400:
            bad_request += 1
        elif status == 500:
            internal_error += 1
        else:
            raise AssertionError(status)
    assert successes > 0, "all output-pressure connections were rejected"
    for writer in writers:
        writer.close()
    await asyncio.gather(*(writer.wait_closed() for writer in writers), return_exceptions=True)
    print(f"output_pressure=16 feed_requests successful={successes} http400={bad_request} http500={internal_error} closed={16-successes-bad_request-internal_error}")

async def health():
    reader, writer = await asyncio.open_connection(HOST, PORT)
    writer.write(b"GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
    await writer.drain()
    status, body = await asyncio.wait_for(response(reader), 5)
    assert status == 200, (status, body)
    assert json.loads(body).get("status") == "ok", body
    writer.close()
    await writer.wait_closed()
    print("post_pressure_health=200")

async def main():
    await output_pressure()
    await input_pressure()
    await health()

asyncio.run(main())
