#!/usr/bin/env python3
"""Keep-alive probe client: N connections, each loops feed -> sleep -> post -> sleep,
tagging every request with an X-Id header, and reports any request whose response does
not arrive within TIMEOUT seconds (with its id, local port and the bytes sent).
usage: probe.py host port conns seconds"""
import asyncio, json, random, sys, time
TOKENS = json.load(open(sys.argv[5])) if len(sys.argv) > 5 else []

HOST, PORT, CONNS, SECONDS = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4])
TIMEOUT = 5.0
lost = []
done = 0


async def read_response(reader):
    head = await reader.readuntil(b"\r\n\r\n")
    n = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            n = int(line.split(b":", 1)[1].strip())
    body = await reader.readexactly(n) if n else b""
    return head, body


async def conn(i):
    global done
    reader, writer = await asyncio.open_connection(HOST, PORT)
    lport = writer.get_extra_info("sockname")[1]
    rid = i * 1_000_000
    end = time.time() + SECONDS
    post_id = 1
    tok = TOKENS[i % len(TOKENS)]["token"].encode() if TOKENS else b""
    while time.time() < end:
        reqs = [b"GET /feed HTTP/1.1\r\nHost: p\r\n", b"GET /posts/%d HTTP/1.1\r\nHost: p\r\n" % post_id]
        if tok and random.random() < 0.15:
            reqs.append(b"POST /posts/%d/like HTTP/1.1\r\nHost: p\r\nAuthorization: Bearer " % post_id + tok + b"\r\nContent-Length: 0\r\n")
        if tok and random.random() < 0.05:
            body = b'{"body":"probe %d %d"}' % (i, rid)
            reqs.append(b"POST /posts HTTP/1.1\r\nHost: p\r\nAuthorization: Bearer " + tok + b"\r\nContent-Type: application/json\r\nContent-Length: %d\r\n" % len(body) + b"X-Body: 1\r\n" + b"\r\n" + body)
        for req0 in reqs:
            rid += 1
            if b"X-Body: 1" in req0:
                req = req0.replace(b"X-Body: 1\r\n", b"X-Id: %d\r\n" % rid)
            else:
                req = req0 + b"X-Id: %d\r\n\r\n" % rid
            path = req.split(b" ")[1]
            writer.write(req)
            await writer.drain()
            t0 = time.time()
            try:
                head, body = await asyncio.wait_for(read_response(reader), TIMEOUT)
            except asyncio.TimeoutError:
                lost.append((rid, lport, req))
                print(f"LOST id={rid} lport={lport} after {time.time()-t0:.1f}s: {req!r} buffered={bytes(reader._buffer)[:400]!r}", flush=True)
                writer.close()
                return
            except Exception as e:
                print(f"ERR id={rid} lport={lport}: {e}", flush=True)
                writer.close()
                return
            done += 1
            if path == b"/feed":
                try:
                    import json
                    posts = json.loads(body)["posts"]
                    post_id = posts[random.randrange(len(posts))]["id"]
                except Exception:
                    pass
            await asyncio.sleep(0.2 + random.random() * 0.4)
    writer.close()


async def main():
    await asyncio.gather(*(conn(i) for i in range(CONNS)))
    print(f"done={done} lost={len(lost)}")


asyncio.run(main())
