#!/usr/bin/env python3
"""Real Linux accept-table exhaustion/reuse. Run in a private network namespace."""
import asyncio, os, pathlib, resource, shutil, subprocess, sys, tempfile

def client_descriptors(need=8192):
    # The client holds 4,097 sockets; the server's own limit is set separately with prlimit.
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    want = need if hard == resource.RLIM_INFINITY else min(hard, need)
    if want < need: raise RuntimeError(f'client needs {need} descriptors; hard limit is {hard}')
    if soft < want: resource.setrlimit(resource.RLIMIT_NOFILE, (want, hard))

async def response(reader):
    head = await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'), 10)
    assert head.startswith(b'HTTP/1.1 200'), head
    size = int(next(x.split(b':',1)[1] for x in head.split(b'\r\n') if x.lower().startswith(b'content-length:')))
    await reader.readexactly(size)

async def main(binary, seed):
    client_descriptors()
    subprocess.run(['ip','link','set','lo','up'],check=True)
    with tempfile.TemporaryDirectory(prefix='accept-live-') as folder:
        db=pathlib.Path(folder)/'feed.db';shutil.copyfile(seed,db)
        env={**os.environ,'SQLITE_PATH':str(db),'JWT_SECRET':'twelve-dollar-challenge','HOST':'127.0.0.1','PORT':'3081'}
        log=open(pathlib.Path(folder)/'server.log','w+')
        p=subprocess.Popen(['prlimit','--nofile=256:256',binary],env=env,stdout=log,stderr=log)
        writers=[]
        request=b'GET /health HTTP/1.1\r\nHost: x\r\n\r\n'
        try:
            for _ in range(600):
                if p.poll() is not None: raise RuntimeError('server exited')
                try:
                    r,w=await asyncio.open_connection('127.0.0.1',3081);w.close();await w.wait_closed();break
                except OSError: await asyncio.sleep(.1)
            else: raise RuntimeError('startup timeout')
            # 16 tables * 256 slots: exercise every handoff, keeping every socket alive.
            for start in range(0,4096,64):
                async def connect():
                    r,w=await asyncio.open_connection('127.0.0.1',3081)
                    w.write(request);await w.drain();await response(r)
                    return r,w
                writers.extend(await asyncio.gather(*(connect() for _ in range(64))))
            r,w=await asyncio.open_connection('127.0.0.1',3081)
            w.write(request);await w.drain()
            pending=asyncio.create_task(response(r))
            await asyncio.sleep(.3)
            assert not pending.done(), 'full tables must leave the new socket queued'
            # Free one slot in the first ring, not the current accepting ring.
            writers[0][1].close();await writers[0][1].wait_closed()
            await asyncio.wait_for(pending,10)
            writers[0]=(r,w)
            for start in range(0,len(writers),128):
                batch=writers[start:start+128]
                for _,w in batch: w.write(request)
                await asyncio.gather(*(response(r) for r,_ in batch))
            print('accept live: 4096 persistent connections, 16 rings, full-table backpressure, noncurrent-ring reuse, all connections healthy')
        finally:
            for _,w in writers: w.close()
            p.terminate()
            try:p.wait(timeout=10)
            except subprocess.TimeoutExpired:p.kill();p.wait()
            log.seek(0);print(log.read());log.close()

asyncio.run(main(str(pathlib.Path(sys.argv[1]).resolve()),sys.argv[2]))
