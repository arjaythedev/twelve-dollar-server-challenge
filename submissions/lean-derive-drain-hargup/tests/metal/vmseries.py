#!/usr/bin/env python3
"""vmseries.py [--every S] GLOB... — the guest time series of a vm153 run: connections, TCP memory,
the app cgroup's memory (current, sock, kernel, anon, file) and socket memory per connection."""
import glob, json, sys
args = sys.argv[1:]
every = 5.0
if args and args[0] == '--every':
    every = float(args[1]); args = args[2:]
for pat in args:
    for d in sorted(glob.glob(pat), key=lambda x: x.rsplit('-', 1)[-1]):
        try:
            g = json.load(open(d + '/guest-samples.json'))
        except (OSError, ValueError):
            continue
        print('===', d.split('/')[-1], len(g), 'samples')
        t0 = g[0]['up'] if g else 0
        last = None
        for x in g:
            alive = x['srv'] is not None
            if last is not None and x['up'] - last < every:
                continue
            last = x['up']
            cg = x.get('cg') or {}
            mb = lambda k: cg.get(k, 0) / 2**20
            conns = x['tcp'].get('inuse', 0)
            spc = cg.get('sock', 0) / conns if conns else 0
            print(f"t={x['up'] - t0:5.0f} {'srv' if alive else 'DEAD'} conns={conns:7d} tcpmem={x['tcp'].get('mem', 0):6d}p "
                  f"cg={mb('current'):6.0f}MB sock={mb('sock'):5.0f} kernel={mb('kernel'):5.0f} anon={mb('anon'):4.0f} "
                  f"file={mb('file'):4.0f} sock/conn={spc:5.0f}B memavail={(x['mem_avail_kB'] or 0) // 1024}MB")
