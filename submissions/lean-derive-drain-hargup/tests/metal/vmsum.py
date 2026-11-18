#!/usr/bin/env python3
"""vmsum.py GLOB... — one line per vm153 run (in start order): verdict (ok: total latency incl. connect;
k6: the official basis, http_req_duration over the whole run), hold latency, guest CPU per
request (busy / system / softirq), the leaderboard's metrics (app cgroup CPU per request and memory
peak) and the network loss counters over the hold, when the run recorded them."""
import glob, json, sys
rows = []
for pat in sys.argv[1:]:
    for d in glob.glob(pat):
        try:
            s = json.load(open(d + '/summary.json'))
        except (OSError, ValueError):
            continue
        rows.append((d.rsplit('-', 1)[-1], s))
rows.sort(key=lambda r: r[0])
def f(v, w=6, p=1):
    return f'{v:{w}.{p}f}' if isinstance(v, (int, float)) else ' ' * (w - 1) + '-'
print(f'{"variant":42s} {"tag":9s} ok k6 {"k6p99w":>7s} {"p95":>7s} {"p99":>7s} {"rps":>6s} {"busy":>5s} {"sys":>5s} {"sirq":>5s} {"cgUs":>5s} {"cg%":>4s} {"peakMB":>6s} {"sockMB":>6s} {"retx/k":>6s} {"tmo":>5s} {"lovf":>5s} {"sndrop":>6s} {"tapdrop":>7s} {"maxSockMB":>9s} {"oom":>3s} {"alive":>5s}')
for _, s in rows:
    net = s.get('guest_net_hold', {})
    mid = s.get('app_cg_hold_mid_mb', {})
    k6w = s["verdict"].get("whole", {}).get("k6", {})
    print(f'{s["variant"]:42s} {s.get("tag", ""):9s} {"P" if s["verdict"]["pass"] else "F"}  {"P" if k6w.get("pass") else "F"}  {f(k6w.get("p99_ms"), 7)} {f(s.get("hold_p95_ms"), 7)} {f(s.get("hold_p99_ms"), 7)} '
          f'{f(s.get("hold_mean_rps"), 6, 0)} {f(s.get("guest_busy_us_per_request"), 5)} {f(s.get("guest_system_us_per_request"), 5)} '
          f'{f(s.get("guest_softirq_us_per_request"), 5)} {f(s.get("app_cg_us_per_request"), 5)} '
          f'{f((s.get("app_cg_cpu_fraction") or 0) * 100 if s.get("app_cg_cpu_fraction") else None, 4, 0)} {f(s.get("app_cg_peak_mb"), 6, 0)} '
          f'{f(mid.get("sock"), 6)} {f(s.get("net_retrans_per_1k_requests"), 6, 2)} {f(net.get("TCPTimeouts"), 5, 0)} '
          f'{f(net.get("ListenOverflows"), 5, 0)} {f(net.get("softnet_dropped"), 6, 0)} {f(s.get("tap_hold", {}).get("tx_dropped"), 7, 0)} {f(s.get("app_cg_max_sock_mb"), 9, 0)} {"Y" if s.get("oom_killed") else "-":>3s} {("Y" if s.get("server_alive_through_hold") else "N") if "server_alive_through_hold" in s else "-":>5s}')
