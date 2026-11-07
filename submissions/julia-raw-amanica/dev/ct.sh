#!/usr/bin/env bash
# Build, test and stress test the submission in a podman container that mimics the box:
# Ubuntu 24.04, 1 CPU, 2 GB RAM, no swap, nofile 65535.
#
#   dev/ct.sh image                         build the dev image (install.sh baked in)
#   dev/ct.sh build                         run build.sh in a clean container
#   dev/ct.sh test                          test/run.sh (build + start + 42 checks), as CI does
#   dev/ct.sh stress [VUS] [DUR]            one k6 run on a fresh server + DB; appends a row to stress-results.csv
#   dev/ct.sh ladder [START] [MAX] [DUR]    double VUS until a run fails, then bisect (like the scoring)
#   dev/ct.sh report                        rewrite the results section of README.md from the CSV
#   dev/ct.sh shell                         interactive shell in the dev image
#
# Env: LABEL (free text for the CSV row, e.g. "epoll baseline"), NOTES, CPUS (1), MEM (2g), RAMP_UP (30s),
#      SECCOMP (unconfined; "default" to test without io_uring), K6_IMAGE.
# Raw output of each run: results/<timestamp>-vus<N>/ (gitignored). The CSV is committed.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CSV="$HERE/stress-results.csv"
IMAGE=localhost/julia-raw-amanica-dev
K6_IMAGE="${K6_IMAGE:-docker.io/grafana/k6:2.3.0@sha256:2dd4756ec6e14e62778916e1eb850108e4b499304c02df56760fa0bdb0fba0b8}"
CPUS="${CPUS:-1}" MEM="${MEM:-2g}" RAMP_UP="${RAMP_UP:-30s}" LABEL="${LABEL:-dev}" NOTES="${NOTES:-}"
SEC=(); [ "${SECCOMP:-unconfined}" = unconfined ] && SEC=(--security-opt seccomp=unconfined)
[ -f "$ROOT/seed/feed.db" ] || { echo "no seed/feed.db: run bash seed/make-seed.sh in $ROOT" >&2; exit 1; }

image() { podman build -q -f "$HERE/dev/Containerfile" -t "$IMAGE" "$HERE" >/dev/null; }
box() {
  podman run --rm --cpus "$CPUS" --memory "$MEM" --memory-swap "$MEM" --ulimit nofile=65535:65535 \
    "${SEC[@]}" -v "$ROOT:/src:ro" -v julia-raw-amanica-dl:/cache:U -e JR_DL_CACHE=/cache "$@"
}

# Samples every 5 s while the server container lives: server CPU % over the window (cgroup usage_usec
# deltas), server cgroup memory (includes page cache), julia RSS, and the k6 container's memory.
sampler() { # sampler <server> <k6> <outfile>
  local srv="$1" k6="$2" prev="" cur now_ns
  while podman container exists "$srv"; do
    cur="$(podman exec "$srv" sh -c 'awk "/usage_usec/{print \$2}" /sys/fs/cgroup/cpu.stat; cat /sys/fs/cgroup/memory.current; ps -o rss= -C julia | head -1' 2>/dev/null | tr '\n' ' ')"
    now_ns="$(date +%s%N)"
    # shellcheck disable=SC2086
    set -- $cur
    if [ -n "$prev" ] && [ -n "${3:-}" ]; then
      # shellcheck disable=SC2086
      set -- $prev "$@"     # prev_usec prev_ns usec mem rss
      k6mem="$(podman stats --no-stream --format '{{.MemUsage}}' "$k6" 2>/dev/null | cut -d' ' -f1)"
      echo "$(date +%T) cpu=$(( ($3 - $1) * 100000 / (now_ns - $2) )) cgmem_mb=$(( $4 >> 20 )) rss_mb=$(( $5 >> 10 )) k6mem=${k6mem:-?}"
      prev="$3 $now_ns"
    elif [ -n "${1:-}" ]; then
      prev="$1 $now_ns"
    fi
    sleep 5
  done
}

csv_header="timestamp,commit,label,julia,backend,transport,cpus,mem,vus,ramp,duration,requests,req_s,p95_ms,p99_ms,max_ms,err_pct,pass,failed_on,srv_cpu_avg,srv_cpu_max,srv_rss_peak_mb,srv_cgmem_peak_mb,k6_mem_peak,boot_ms,notes"

# One run. Sets RUN_PASS=1/0.
stress_run() { # stress_run <vus> <duration>
  local vus="$1" dur="$2" out name k6name rc=0 statpid
  out="$HERE/results/$(date +%Y%m%d-%H%M%S)-vus$vus"; mkdir -p "$out"
  name="jr-srv-$$-$vus"; k6name="jr-k6-$$-$vus"
  # shellcheck disable=SC2064
  trap "podman rm -f $name $k6name >/dev/null 2>&1 || true" EXIT
  box -d --name "$name" --sysctl net.ipv4.ip_local_port_range="1024 65535" \
    "$IMAGE" bash /src/submissions/julia-raw-amanica/dev/in-container.sh serve >/dev/null
  for _ in $(seq 600); do
    podman exec "$name" curl -sf http://127.0.0.1:3000/health >/dev/null 2>&1 && break
    podman container exists "$name" || { echo "server container died" >&2; return 1; }
    sleep 1
  done
  podman exec "$name" curl -sf http://127.0.0.1:3000/health >/dev/null || { podman logs "$name" | tail -30; return 1; }
  echo "== VUS=$vus RAMP_UP=$RAMP_UP DURATION=$dur -> ${out#"$HERE"/}"
  sampler "$name" "$k6name" > "$out/stats.log" 2>&1 &
  statpid=$!
  podman run --rm --name "$k6name" --user 0 --network "container:$name" \
    --ulimit nofile="$(ulimit -Hn)":"$(ulimit -Hn)" \
    -v "$ROOT/bench:/bench:ro" -v "$ROOT/seed/tokens.json:/seed/tokens.json:ro" -v "$out:/out" \
    "$K6_IMAGE" run -q --no-color -e VUS="$vus" -e DURATION="$dur" -e RAMP_UP="$RAMP_UP" \
    -e TOKENS=/seed/tokens.json --summary-export /out/summary.json /bench/load.js > "$out/k6.txt" 2>&1 || rc=$?
  podman logs "$name" > "$out/server.log" 2>&1 || true
  podman rm -f "$name" >/dev/null 2>&1 || true
  wait "$statpid" 2>/dev/null || true
  trap - EXIT

  [ -f "$CSV" ] || echo "$csv_header" > "$CSV"
  local commit; commit="$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo none)"
  git -C "$HERE" status --porcelain -- . 2>/dev/null | grep -q . && commit="$commit+dirty"
  local row
  row="$(python3 -I - "$out" "$rc" <<'PY'
import json, re, sys, os
out, rc = sys.argv[1], int(sys.argv[2])
def num(x, d=3): return "" if x is None else f"{x:.{d}f}"
s = {}
try: s = json.load(open(os.path.join(out, "summary.json")))["metrics"]
except Exception: pass
dur = s.get("http_req_duration", {}); fail = s.get("http_req_failed", {}); reqs = s.get("http_reqs", {})
failed_on = [n for m in ("http_req_duration", "http_req_failed")
             for n, crossed in s.get(m, {}).get("thresholds", {}).items() if crossed]
cpu, cg, rss, k6 = [], [], [], []
for line in open(os.path.join(out, "stats.log")):
    m = dict(re.findall(r"(\w+)=(\S+)", line))
    if "cpu" in m: cpu.append(int(m["cpu"])); cg.append(int(m["cgmem_mb"])); rss.append(int(m["rss_mb"]))
    if m.get("k6mem", "?") != "?": k6.append(m["k6mem"])
def k6v(v):
    m = re.match(r"([\d.]+)([kMG]?B)", v); f = {"B": 1e-6, "kB": 1e-3, "MB": 1, "GB": 1e3}
    return float(m.group(1)) * f[m.group(2)] if m else 0
log = open(os.path.join(out, "server.log")).read()
kv = dict(re.findall(r"(\w+)=(\S+)", log.split("listening")[0][-300:])) if "listening" in log else {}
busy = [c for c in cpu if c > 2] or cpu
print(",".join([
    kv.get("julia", ""), kv.get("backend", ""), kv.get("transport", ""),
    str(reqs.get("count", "")), num(reqs.get("rate"), 1),
    num(dur.get("p(95)")), num(dur.get("p(99)")), num(dur.get("max"), 1),
    num(100 * fail["value"], 3) if "value" in fail else "",
    "1" if rc == 0 and s else "0",
    ("k6 exit %d" % rc if not s else " ".join(failed_on)).replace(",", ";"),
    str(round(sum(busy) / len(busy))) if busy else "", str(max(cpu)) if cpu else "",
    str(max(rss)) if rss else "", str(max(cg)) if cg else "",
    ("%.0fMB" % max(map(k6v, k6))) if k6 else "",
    (re.findall(r"boot_ms=(\d+)", log) or [""])[0],
]))
PY
)"
  IFS=, read -r jv be tr req rps p95 p99 mx err pass failed_on cavg cmax rss cg k6m boot <<< "$row"
  echo "$(date -u +%FT%TZ),$commit,${LABEL//,/;},$jv,$be,$tr,$CPUS,$MEM,$vus,$RAMP_UP,$dur,$req,$rps,$p95,$p99,$mx,$err,$pass,$failed_on,$cavg,$cmax,$rss,$cg,$k6m,$boot,${NOTES//,/;}" >> "$CSV"
  printf '   %s  boot=%sms req/s=%s p95=%sms p99=%sms err=%s%% cpu avg/max=%s/%s%% rss=%sMB k6=%s %s\n' \
    "$([ "$pass" = 1 ] && echo PASS || echo FAIL)" "$boot" "$rps" "$p95" "$p99" "$err" "$cavg" "$cmax" "$rss" "$k6m" "$failed_on"
  if [ "$pass" != 1 ] && [ -n "$cmax" ] && [ "$cmax" -lt 85 ]; then
    echo "   note: server CPU never above 85%: the limit may be the load generator or a connection limit, not the server"
  fi
  RUN_PASS="$pass"
}

ladder() { # ladder <start> <max> <dur>
  local vus="$1" max="$2" dur="$3" lo=0 hi=0 tol mid
  while [ "$vus" -le "$max" ]; do
    stress_run "$vus" "$dur"
    if [ "$RUN_PASS" = 1 ]; then lo="$vus"; vus=$((vus * 2)); else hi="$vus"; break; fi
  done
  if [ "$hi" = 0 ]; then echo "== passed up to $lo VUS (MAX=$max reached)"; return; fi
  while :; do                                  # narrow down to within 5% (min 250 VUS)
    tol=$(( lo / 20 )); [ "$tol" -lt 250 ] && tol=250
    [ $((hi - lo)) -le "$tol" ] && break
    mid=$(( (lo + hi) / 2 / 50 * 50 ))
    { [ "$mid" -le "$lo" ] || [ "$mid" -ge "$hi" ]; } && break
    stress_run "$mid" "$dur"
    if [ "$RUN_PASS" = 1 ]; then lo="$mid"; else hi="$mid"; fi
  done
  echo "== highest passing: $lo VUS (lowest failing: $hi)"
}

report() {
  [ -f "$CSV" ] || { echo "no $CSV yet"; return 1; }
  python3 -I - "$CSV" "$HERE/README.md" <<'PY'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
best = {}
for r in rows:
    if r["pass"] == "1":
        k = r["label"]
        if k not in best or int(r["vus"]) > int(best[k]["vus"]): best[k] = r
L = ["Local runs, `dev/ct.sh` (podman: server 1 CPU / 2 GB; k6 on the same host, so numbers are only",
     "comparable with each other, not with the official leaderboard). Source: [stress-results.csv](stress-results.csv).", "",
     "**Best passing run per label**", "",
     "| label | commit | VUS | req/s | p95 ms | p99 ms | err % | server CPU avg | RSS MB | boot ms |",
     "|---|---|--:|--:|--:|--:|--:|--:|--:|--:|"]
for r in sorted(best.values(), key=lambda r: int(r["vus"]), reverse=True):
    L.append(f"| {r['label']} | {r['commit']} | {r['vus']} | {r['req_s']} | {r['p95_ms']} | {r['p99_ms']} | {r['err_pct']} | {r['srv_cpu_avg']}% | {r['srv_rss_peak_mb']} | {r.get('boot_ms') or '–'} |")
L += ["", "**Last 20 runs**", "",
      "| when (UTC) | label | VUS | dur | req/s | p95 ms | p99 ms | err % | CPU avg/max | result |",
      "|---|---|--:|---|--:|--:|--:|--:|--:|---|"]
for r in rows[-20:][::-1]:
    res = "pass" if r["pass"] == "1" else "FAIL " + r["failed_on"]
    L.append(f"| {r['timestamp'][:16].replace('T',' ')} | {r['label']} | {r['vus']} | {r['duration']} | {r['req_s']} | {r['p95_ms']} | {r['p99_ms']} | {r['err_pct']} | {r['srv_cpu_avg']}/{r['srv_cpu_max']}% | {res} |")
readme = open(sys.argv[2]).read()
b, e = "<!-- results:begin -->", "<!-- results:end -->"
block = b + "\n" + "\n".join(L) + "\n" + e
if b in readme: readme = readme[:readme.index(b)] + block + readme[readme.index(e) + len(e):]
else: readme = readme.rstrip() + "\n\n## Local stress results\n\n" + block + "\n"
open(sys.argv[2], "w").write(readme)
print("README.md updated:", len(rows), "runs,", len(best), "labels")
PY
}

cmd="${1:-test}"; shift || true
case "$cmd" in
  image) image; echo "$IMAGE" ;;
  build|test) image; box "$IMAGE" bash /src/submissions/julia-raw-amanica/dev/in-container.sh "$cmd" ;;
  shell) image; box -it "$IMAGE" bash ;;
  stress) image; stress_run "${1:-1000}" "${2:-1m}"; [ "$RUN_PASS" = 1 ] ;;
  ladder) image; ladder "${1:-2500}" "${2:-320000}" "${3:-1m}" ;;
  report) report ;;
  *) sed -n '2,18p' "$0"; exit 2 ;;
esac
