import argparse
import asyncio
import json
import resource
import time
import random
from urllib.parse import urlsplit

p = argparse.ArgumentParser()
p.add_argument('url')
p.add_argument('--mode', choices=['idle', 'partial', 'partial-body'], default='idle')
p.add_argument('--count', type=int, default=15000)
p.add_argument('--hold', type=float, default=20)
p.add_argument('--ports', help='comma-separated forwarding ports for this same server')
p.add_argument('--timeout', type=float, default=300, help='whole-test wall-time limit in seconds')
args = p.parse_args()
soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
resource.setrlimit(resource.RLIMIT_NOFILE, (min(65536, hard), hard))
target = urlsplit(args.url)
ports = [int(port) for port in args.ports.split(',')] if args.ports else [target.port or 80]
request = b'GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n'
partial_body = (b'POST /posts HTTP/1.1\r\nHost: localhost\r\nContent-Length: 16384\r\n\r\n'
                b'{"body":"'+b'x'*12000)

async def get_response(reader):
    head = await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'), 30)
    length = int(next(line.split(b':',1)[1] for line in head.split(b'\r\n') if line.lower().startswith(b'content-length:')))
    return head.split()[1], await reader.readexactly(length)

async def main():
    connections, errors = [], []
    refresh_tasks, refresh_errors = [], []
    finished = asyncio.Event()
    semaphore = asyncio.Semaphore(64)
    start = time.monotonic()
    async def refresh(reader,writer):
        # Refresh early sockets while batches open; one reader owns each socket.
        try:
            while True:
                try:
                    await asyncio.wait_for(finished.wait(),random.uniform(25,35))
                    return
                except asyncio.TimeoutError: pass
                writer.write(request)
                await writer.drain()
                status, body = await get_response(reader)
                assert status == b'200', (status,body)
        except Exception as exc:
            refresh_errors.append(str(exc))
    async def connect(index):
        async with semaphore:
            writer = None
            try:
                reader, writer = await asyncio.wait_for(asyncio.open_connection(target.hostname, ports[index % len(ports)]), 30)
                writer.write(request if args.mode == 'idle' else partial_body if args.mode == 'partial-body'
                             else b'GET /health HTTP/1.1\r\nHost: localhost')
                await writer.drain()
                if args.mode == 'idle':
                    status, body = await get_response(reader)
                    assert status == b'200', (status, body)
                    refresh_tasks.append(asyncio.create_task(refresh(reader,writer)))
                connections.append((reader,writer))
            except Exception as exc:
                errors.append(str(exc))
                if writer is not None: writer.close()
    await asyncio.gather(*(connect(i) for i in range(args.count)))
    print(json.dumps({'mode':args.mode,'requested':args.count,'connected':len(connections),
                      'errors':len(errors),'connect_seconds':time.monotonic()-start}),flush=True)
    await asyncio.sleep(args.hold)
    finished.set()
    await asyncio.gather(*refresh_tasks)
    # TCP resets set a reader exception without necessarily setting EOF.
    closed = sum(reader.at_eof() or reader.exception() is not None for reader,_ in connections)
    for _, writer in connections: writer.close()
    await asyncio.gather(*(writer.wait_closed() for _,writer in connections), return_exceptions=True)
    await asyncio.sleep(2)
    reader, writer = await asyncio.open_connection(target.hostname,target.port or 80)
    writer.write(request)
    await writer.drain()
    status, body = await get_response(reader)
    writer.close()
    await writer.wait_closed()
    print(json.dumps({'closed_during_hold':closed,'refresh_errors':len(refresh_errors),'recovery_status':status.decode(),
                      'recovery_body':json.loads(body),'errors_sample':errors[:3]}),flush=True)
    assert status == b'200'
    reader, writer = await asyncio.open_connection(target.hostname,target.port or 80)
    writer.write(b'GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n')
    await writer.drain()
    status, body = await get_response(reader)
    writer.close()
    await writer.wait_closed()
    assert status == b'200' and len(json.loads(body)['posts']) == 20
    print('feed recovery 200',flush=True)
    if args.mode == 'idle': assert not errors and not closed and not refresh_errors

async def bounded_main():
    await asyncio.wait_for(main(),args.timeout)

asyncio.run(bounded_main())
