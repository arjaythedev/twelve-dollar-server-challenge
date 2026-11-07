"""Turns one run's raw output (results/<run>/) into a row of stress-results.csv and a console line.

usage: python3 -I dev/record.py <run dir> <generator exit code> <csv>
env:   GEN LABEL NOTES CPUS MEM VUS RAMP DUR COMMIT
Exit status 0 = the run passed the scoring thresholds (p95 < 500 ms, p99 < 1 s, errors < 1%).
"""
import csv, datetime, json, os, re, sys

COLUMNS = ["timestamp", "commit", "label", "gen", "julia", "backend", "transport", "cpus", "mem",
           "vus", "ramp", "duration", "requests", "req_s", "est_users", "p95_ms", "p99_ms", "max_ms",
           "err_pct", "pass", "failed_on", "srv_cpu_avg", "srv_cpu_max", "srv_rss_peak_mb",
           "srv_cgmem_peak_mb", "gen_mem_peak", "boot_ms", "notes"]
RENAMED = {"k6_mem_peak": "gen_mem_peak"}
USERS_PER_REQ_S = 10.6   # leaderboard: 310k users <-> 29.3k req/s


def f(x, d=3):
    return "" if x is None else f"{x:.{d}f}"


def read(path):
    try:
        return open(path).read()
    except OSError:
        return ""


out, rc, csv_path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
env = os.environ
gen = env.get("GEN", "k6")
row = {c: "" for c in COLUMNS}
row.update(timestamp=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
           commit=env.get("COMMIT", ""), label=env.get("LABEL", ""), gen=gen, cpus=env.get("CPUS", ""),
           mem=env.get("MEM", ""), vus=env.get("VUS", ""), ramp=env.get("RAMP", "") if gen == "k6" else "",
           duration=env.get("DUR", ""), notes=env.get("NOTES", ""))

ok = False
failed = []
if gen == "k6":
    try:
        m = json.load(open(os.path.join(out, "summary.json")))["metrics"]
    except Exception:
        m = None
    if m:
        dur, fail, reqs = m.get("http_req_duration", {}), m.get("http_req_failed", {}), m.get("http_reqs", {})
        row.update(requests=reqs.get("count", ""), req_s=f(reqs.get("rate"), 1), p95_ms=f(dur.get("p(95)")),
                   p99_ms=f(dur.get("p(99)")), max_ms=f(dur.get("max"), 1),
                   err_pct=f(100 * fail["value"]) if "value" in fail else "")
        failed = [n for k in ("http_req_duration", "http_req_failed")
                  for n, crossed in m.get(k, {}).get("thresholds", {}).items() if crossed]
        ok = rc == 0
    else:
        failed = [f"k6 exit {rc}"]
else:
    js = re.findall(r"BLAST_JSON (\{.*\})", read(os.path.join(out, "wrk.txt")))
    if js:
        b = json.loads(js[-1])
        secs = b["duration_us"] / 1e6
        errs = sum(b[k] for k in ("err_status", "err_connect", "err_read", "err_write", "err_timeout"))
        rps = b["requests"] / secs if secs else 0
        p95, p99, err = b["p95_us"] / 1000, b["p99_us"] / 1000, 100 * errs / max(b["requests"], 1)
        row.update(requests=b["requests"], req_s=f(rps, 1), est_users=str(round(rps * USERS_PER_REQ_S)),
                   p95_ms=f(p95), p99_ms=f(p99), max_ms=f(b["max_us"] / 1000, 1), err_pct=f(err))
        failed = ([f"p(95)<500"] if p95 >= 500 else []) + ([f"p(99)<1000"] if p99 >= 1000 else []) + \
                 ([f"errors<1%"] if err >= 1 else [])
        ok = rc == 0 and not failed and b["requests"] > 0
    else:
        failed = [f"wrk exit {rc}"]
row["pass"] = "1" if ok else "0"
row["failed_on"] = " ".join(failed)

cpu, cg, rss, gm = [], [], [], []
for line in read(os.path.join(out, "stats.log")).splitlines():
    kv = dict(re.findall(r"(\w+)=(\S+)", line))
    if "cpu" in kv:
        cpu.append(int(kv["cpu"])); cg.append(int(kv["cgmem_mb"])); rss.append(int(kv["rss_mb"]))
    mm = re.match(r"([\d.]+)([kMG]?B)", kv.get("genmem", ""))
    if mm:
        gm.append(float(mm.group(1)) * {"B": 1e-6, "kB": 1e-3, "MB": 1, "GB": 1e3}[mm.group(2)])
busy = [c for c in cpu if c > 2] or cpu
log = read(os.path.join(out, "server.log"))
info = dict(re.findall(r"(\w+)=(\S+)", log.split("listening")[0][-300:])) if "listening" in log else {}
row.update(julia=info.get("julia", ""), backend=info.get("backend", ""), transport=info.get("transport", ""),
           srv_cpu_avg=str(round(sum(busy) / len(busy))) if busy else "", srv_cpu_max=str(max(cpu)) if cpu else "",
           srv_rss_peak_mb=str(max(rss)) if rss else "", srv_cgmem_peak_mb=str(max(cg)) if cg else "",
           gen_mem_peak=f"{max(gm):.0f}MB" if gm else "", boot_ms=(re.findall(r"boot_ms=(\d+)", log) or [""])[0])

# append, migrating an older header (missing / renamed columns) if needed
rows = []
if os.path.exists(csv_path):
    with open(csv_path) as fh:
        rows = [{RENAMED.get(k, k): v for k, v in r.items()} for r in csv.DictReader(fh)]
    for r in rows:
        r.setdefault("gen", "k6")
        if not r["gen"]:
            r["gen"] = "k6"
rows.append(row)
with open(csv_path, "w", newline="") as fh:
    w = csv.DictWriter(fh, fieldnames=COLUMNS, extrasaction="ignore", lineterminator="\n")
    w.writeheader()
    for r in rows:
        w.writerow({c: r.get(c, "") for c in COLUMNS})

what = "users" if gen == "k6" else "connections"
print(f"   {'PASS' if ok else 'FAIL'}  {gen} {row['vus']} {what}  boot={row['boot_ms']}ms req/s={row['req_s']}"
      + (f" (~{row['est_users']} users)" if row["est_users"] else "")
      + f" p95={row['p95_ms']}ms p99={row['p99_ms']}ms err={row['err_pct']}%"
      + f" cpu avg/max={row['srv_cpu_avg']}/{row['srv_cpu_max']}% rss={row['srv_rss_peak_mb']}MB"
      + f" {gen}={row['gen_mem_peak']} {row['failed_on']}")
if not ok and cpu and max(cpu) < 85:
    print("   note: server CPU never above 85%: the limit may be the load generator, the host or a "
          "connection limit, not the server")
sys.exit(0 if ok else 1)
