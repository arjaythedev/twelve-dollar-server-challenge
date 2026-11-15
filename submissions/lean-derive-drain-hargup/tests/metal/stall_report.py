#!/usr/bin/env python3
"""stall_report.py RESULT_DIR... — per-second view around an injected stall: requests answered,
p99, and the server's CPU seconds spent recovering (10 s after the stall) versus 10 s before."""
import bisect, datetime, json, sys
for d in sys.argv[1:]:
    s = json.load(open(d + '/summary.json'))
    L = json.load(open(d + '/load.json'))
    at, dur = s['stall_at'], s['stall_for']
    t0 = datetime.datetime.fromisoformat(L['started_at'].replace('Z', '+00:00')).timestamp()
    cpu = json.load(open(d + '/cpu-samples.json'))
    ts = [c[0] for c in cpu]
    def cpu_at(t):
        i = bisect.bisect_left(ts, t0 + t)
        return cpu[min(i, len(cpu) - 1)][1] / 1e6
    print(d.rsplit('/', 1)[-1], f"stall {at}s+{dur}s")
    print('   t   reqs   p99_ms')
    for t in L['timeline']:
        if at - 2 <= t['t'] <= at + 12:
            print(f"{t['t']:5} {t['reqs']:6} {t['p99_ms']:8.1f}")
    rec = cpu_at(at + dur + 10) - cpu_at(at + dur)
    before = cpu_at(at) - cpu_at(at - 10)
    print(f"  server CPU s: 10 s before {before:.2f}, 10 s after the stall {rec:.2f}; "
          f"hold p99 {s['hold_p99_ms']} ms; peak MB {s['memory_peak_bytes'] / 2**20:.0f}; max tcp mem pages {s.get('max_tcp_mem_pages')}")
