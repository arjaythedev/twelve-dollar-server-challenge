#!/usr/bin/env python3
"""summarize153.py RESULT_DIR... — one markdown row per bench153 run (raw data: summary.json and
load.json in each directory). Verdicts: p95 < 500 ms, p99 < 1 s, errors < 1 %, for the whole run
and the hold window; "total" latency includes connecting (the generator's own, stricter basis),
"k6" excludes it as k6's http_req_duration does."""
import json, sys
print('| run | variant | users | quota | hold s | whole (total / k6) | hold (total) | hold p95 / p99 ms | worst 1 s p99 ms | CPU µs/req (hold) | req/s (hold) | errors (connect/timeout/reset) | max / min-hold conns | cgroup peak MB | min host MemAvailable GB | client / other cores |')
print('|---|---|---:|---|---:|---|---|---|---:|---:|---:|---|---|---:|---:|---|')
for d in sys.argv[1:]:
    try:
        s = json.load(open(d.rstrip('/') + '/summary.json'))
    except (OSError, ValueError):
        print(f'| {d.rsplit("/", 1)[-1]} | (no summary: run failed) |'); continue
    v = s['verdict']
    ek = s['whole_errors_by_kind']
    errs = f"{s['errors']} ({ek.get('connect', 0)}/{ek.get('timeout', 0)}/{ek.get('reset_eof', 0)})"
    cc = s['counters'].get('connect_errors', {})
    if any(cc.values()):
        errs += ' ' + ','.join(f'{k}={n}' for k, n in cc.items() if n)
    print(f"| {d.rstrip('/').rsplit('/', 1)[-1].rsplit('-', 1)[-1][-6:]} | {s['variant']} | {s['vus']:,} | {s.get('cpu_quota') or '—'} | {s['duration']} "
          f"| {'PASS' if v['whole']['total']['pass'] else 'FAIL'} / {'PASS' if v['whole']['k6']['pass'] else 'FAIL'} | {'PASS' if v['hold']['total']['pass'] else 'FAIL'} "
          f"| {s['hold_p95_ms']:.1f} / {s['hold_p99_ms']:.1f} | {s['hold_worst_second_p99_ms']:.0f} "
          f"| {s['hold_cpu_us_per_request']:.2f} | {s['hold_mean_rps']:,.0f} | {errs} "
          f"| {s['max_conns']:,} / {s.get('min_hold_conns') or 0:,} | {s['memory_peak_bytes'] / 2**20:.0f} "
          f"| {s.get('min_MemAvailable_kB', 0) / 2**20:.1f} | {s.get('hold_client_cores', 0):.2f} / {s.get('hold_other_host_cores', 0):.2f} |")
