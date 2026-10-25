import asyncio, json, sys

PORT=int(sys.argv[1]); COUNT=15000
REQUEST=b'GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n'

async def health(reader, writer):
    writer.write(REQUEST)
    await writer.drain()
    header=await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'),10)
    assert header.split(b'\r\n')[0].split()[1]==b'200',header
    fields=dict(line.split(b':',1) for line in header.lower().split(b'\r\n')[1:] if b':' in line)
    assert fields.get(b'connection',b'').strip()!=b'close',header
    body=await asyncio.wait_for(reader.readexactly(int(fields[b'content-length'])),10)
    assert json.loads(body)['db']=='ok',body

async def main():
    connections=[]
    semaphore=asyncio.Semaphore(256)
    async def connect():
        async with semaphore:
            reader,writer=await asyncio.wait_for(asyncio.open_connection('127.0.0.1',PORT,limit=4096),10)
            connections.append((reader,writer))
            await health(reader,writer)
    async def verify(connection):
        async with semaphore:
            await health(*connection)
    try:
        await asyncio.gather(*(connect() for _ in range(COUNT)))
        print(json.dumps({'ready':True,'connections':len(connections)}),flush=True)
        command=await asyncio.to_thread(sys.stdin.readline)
        assert command.strip()=='verify',command
        await asyncio.gather(*(verify(connection) for connection in connections))
        print(json.dumps({'verified':len(connections),'same_sockets':True}),flush=True)
        command=await asyncio.to_thread(sys.stdin.readline)
        assert command.strip()=='stop',command
    finally:
        for _,writer in connections:writer.close()
        await asyncio.gather(*(writer.wait_closed() for _,writer in connections),return_exceptions=True)

asyncio.run(main())
