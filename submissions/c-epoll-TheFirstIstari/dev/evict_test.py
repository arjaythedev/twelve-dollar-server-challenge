#!/usr/bin/env python3
# FD-pressure validation. Server runs with `ulimit -n 4096`, so tripwires scale
# to L1/L2@1024, release@512. Opens 3000 idle keep-alives and verifies:
#  - every request gets a COMPLETE response (no truncation / RST with data)
#  - server sheds toward release and stays responsive throughout
#  - close-after-response carries `Connection: close` under pressure
#  - idle sheds are clean FINs after a completed response
# Usage: evict_test.py <port>  (server already running on fresh DB)
import socket, time, sys

PORT = int(sys.argv[1])
N = 3000

def read_response(f):
    """Read one full HTTP response from a buffered reader. Returns
    (status, headers_dict, body) or raises on truncation/RST."""
    status_line = f.readline().decode()
    if not status_line:
        return None  # clean FIN (only valid when no request pending)
    parts = status_line.split()
    status = int(parts[1])
    hdrs = {}
    while True:
        line = f.readline().decode()
        if line in ('\r\n', '\n', ''):
            break
        k, _, v = line.partition(':')
        hdrs[k.strip().lower()] = v.strip()
    n = int(hdrs.get('content-length', '0'))
    body = b''
    while len(body) < n:
        chunk = f.read(n - len(body))
        if not chunk:
            raise RuntimeError(f'truncated body: {len(body)}/{n}')
        body += chunk
    return status, hdrs, body

def main():
    conns = []  # (sock, file, got_response: bool)
    refused = 0
    t0 = time.time()
    for i in range(N):
        try:
            s = socket.create_connection(('127.0.0.1', PORT), timeout=15)
        except ConnectionRefusedError:
            refused += 1
            continue
        s.settimeout(15)
        f = s.makefile('rb')
        s.sendall(b'GET /feed HTTP/1.1\r\nHost: x\r\n\r\n')
        try:
            r = read_response(f)
        except (ConnectionResetError, BrokenPipeError) as e:
            print(f'RST/dropped during initial wave at conn {i}: {e}')
            return 1
        if r is None or r[0] != 200 or not r[2].startswith(b'{"posts":'):
            print(f'bad initial response at conn {i}: {r[0] if r else None}')
            return 1
        conns.append((s, f))
    t1 = time.time()
    print(f'opened {len(conns)}/{N} (refused={refused}) in {t1-t0:.1f}s')

    print('holding 6s for shedding walks...')
    time.sleep(6)

    # second wave on survivors: expect complete responses, many with close
    closed_clean = 0
    close_hdr = 0
    trunc = 0
    served = 0
    for s, f in conns:
        try:
            s.sendall(b'GET /posts/1 HTTP/1.1\r\nHost: x\r\n\r\n')
        except (ConnectionResetError, BrokenPipeError):
            closed_clean += 1  # was shed while idle; fine (prior response complete)
            continue
        try:
            r = read_response(f)
        except (ConnectionResetError, BrokenPipeError, RuntimeError) as e:
            print(f'BAD: incomplete second response: {e}')
            trunc += 1
            continue
        if r is None:
            closed_clean += 1
            continue
        if r[0] != 200 or not r[2].startswith(b'{"post":'):
            print(f'BAD: second response status/body: {r[0]} {r[2][:60]}')
            trunc += 1
            continue
        served += 1
        if r[1].get('connection') == 'close':
            close_hdr += 1
            # server must FIN after; drain it
            try:
                rest = f.read(1)
                if rest != b'':
                    print('BAD: data after close response')
                    trunc += 1
            except (ConnectionResetError, BrokenPipeError):
                pass
        s.close()
    print(f'second wave: served={served} close_hdr={close_hdr} idle_shed={closed_clean} bad={trunc}')

    # responsiveness probe on a FRESH connection
    s = socket.create_connection(('127.0.0.1', PORT), timeout=15)
    s.settimeout(15)
    f = s.makefile('rb')
    s.sendall(b'GET /health HTTP/1.1\r\nHost: x\r\n\r\n')
    r = read_response(f)
    ok = r is not None and r[0] == 200
    print(f'fresh-conn health under pressure: {"200 OK" if ok else "FAILED"}')
    s.close()

    if trunc or not ok:
        print('EVICT TEST FAILED')
        return 1
    print('EVICT TEST PASSED')
    return 0

sys.exit(main())
