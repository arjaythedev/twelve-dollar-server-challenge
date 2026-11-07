#!/usr/bin/env bash
# Build, test and stress test the submission in podman containers that mimic the box:
# Ubuntu 24.04, 1 CPU, 2 GB RAM, no swap, nofile 65535. See README.md "Running locally".
#
#   dev/ct.sh image                       build the dev images
#   dev/ct.sh build                       run build.sh in a clean container
#   dev/ct.sh test                        test/run.sh (build + start + 42 checks), as CI does
#   dev/ct.sh stress [N] [DUR]            one run on a fresh server + DB; appends a row to stress-results.csv
#   dev/ct.sh ladder [START] [MAX] [DUR]  double N until a run fails, then bisect to within 5%
#   dev/ct.sh report                      rewrite the results section of README.md from the CSV
#   dev/ct.sh shell                       interactive shell in the dev image
#
# GEN picks the load generator:
#   GEN=k6     (default) bench/load.js, the real user model. N = virtual users. Costs ~0.3 MB per user.
#   GEN=blast  wrk with the same request mix and no think time. N = open connections. Cheap; measures
#              max req/s (users estimated as req/s x 10.6).
# Env: LABEL (free text for the CSV row), NOTES, CPUS (1), MEM (2g), RAMP_UP (30s, k6 only),
#      WARMUP (5s, blast only), WRK_THREADS (4), SECCOMP (unconfined; "default" to test without io_uring).
# Raw output of each run: results/<timestamp>-<gen><N>/ (gitignored). The CSV is committed.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CSV="$HERE/stress-results.csv"
IMAGE=localhost/julia-raw-amanica-dev
WRK_IMAGE=localhost/julia-raw-amanica-wrk
K6_IMAGE="${K6_IMAGE:-docker.io/grafana/k6:2.3.0@sha256:2dd4756ec6e14e62778916e1eb850108e4b499304c02df56760fa0bdb0fba0b8}"
GEN="${GEN:-k6}" CPUS="${CPUS:-1}" MEM="${MEM:-2g}" RAMP_UP="${RAMP_UP:-30s}" WARMUP="${WARMUP:-5s}"
WRK_THREADS="${WRK_THREADS:-4}" LABEL="${LABEL:-dev}" NOTES="${NOTES:-}"
case "$GEN" in k6|blast) ;; *) echo "GEN must be k6 or blast" >&2; exit 2 ;; esac
SEC=(); [ "${SECCOMP:-unconfined}" = unconfined ] && SEC=(--security-opt seccomp=unconfined)
[ -f "$ROOT/seed/feed.db" ] || { echo "no seed/feed.db: run bash seed/make-seed.sh in $ROOT" >&2; exit 1; }

image() {
  podman build -q -f "$HERE/dev/Containerfile" -t "$IMAGE" "$HERE" >/dev/null
  [ "$GEN" = blast ] && podman build -q -f "$HERE/dev/Containerfile.wrk" -t "$WRK_IMAGE" "$HERE/dev" >/dev/null
  true
}
box() {
  podman run --rm --cpus "$CPUS" --memory "$MEM" --memory-swap "$MEM" --ulimit nofile=65535:65535 \
    "${SEC[@]}" -v "$ROOT:/src:ro" -v julia-raw-amanica-dl:/cache:U -e JR_DL_CACHE=/cache "$@"
}

# Samples every 2 s while the server container lives: server CPU % over the window (cgroup usage_usec
# deltas), server cgroup memory (includes page cache), julia RSS, and the generator container's memory.
sampler() { # sampler <server> <generator>
  local srv="$1" g="$2" prev="" cur now_ns gm
  while podman container exists "$srv"; do
    cur="$(podman exec "$srv" sh -c 'awk "/usage_usec/{print \$2}" /sys/fs/cgroup/cpu.stat; cat /sys/fs/cgroup/memory.current; ps -o rss= -C julia | head -1' 2>/dev/null | tr '\n' ' ')"
    now_ns="$(date +%s%N)"
    # shellcheck disable=SC2086
    set -- $cur
    if [ -n "$prev" ] && [ -n "${3:-}" ]; then
      # shellcheck disable=SC2086
      set -- $prev "$@"     # prev_usec prev_ns usec mem rss
      gm="$(podman stats --no-stream --format '{{.MemUsage}}' "$g" 2>/dev/null | cut -d' ' -f1)"
      echo "$(date +%T) cpu=$(( ($3 - $1) * 100000 / (now_ns - $2) )) cgmem_mb=$(( $4 >> 20 )) rss_mb=$(( $5 >> 10 )) genmem=${gm:-?}"
      prev="$3 $now_ns"
    elif [ -n "${1:-}" ]; then
      prev="$1 $now_ns"
    fi
    sleep 2
  done
}

run_k6() { # run_k6 <server> <name> <n> <dur> <out>
  podman run --rm --name "$2" --user 0 --network "container:$1" \
    --ulimit nofile="$(ulimit -Hn)":"$(ulimit -Hn)" \
    -v "$ROOT/bench:/bench:ro" -v "$ROOT/seed/tokens.json:/seed/tokens.json:ro" -v "$5:/out" \
    "$K6_IMAGE" run -q --no-color -e VUS="$3" -e DURATION="$4" -e RAMP_UP="$RAMP_UP" \
    -e TOKENS=/seed/tokens.json --summary-export /out/summary.json /bench/load.js > "$5/k6.txt" 2>&1
}

run_blast() { # run_blast <server> <name> <n> <dur> <out>
  local t="$WRK_THREADS"; [ "$3" -lt "$t" ] && t="$3"
  local args=(--rm --name "$2" --network "container:$1" --ulimit nofile="$(ulimit -Hn)":"$(ulimit -Hn)"
              -v "$HERE/dev/blast.lua:/blast.lua:ro" -v "$ROOT/seed/tokens.json:/seed/tokens.json:ro")
  # warm-up (JIT, page cache), not recorded
  podman run "${args[@]}" "$WRK_IMAGE" -t "$t" -c "$3" -d "$WARMUP" --timeout 10s -s /blast.lua \
    http://127.0.0.1:3000 > "$5/wrk-warmup.txt" 2>&1 || true
  podman run "${args[@]}" "$WRK_IMAGE" -t "$t" -c "$3" -d "$4" --timeout 10s -s /blast.lua \
    http://127.0.0.1:3000 > "$5/wrk.txt" 2>&1
}

# One run. Sets RUN_PASS=1/0.
stress_run() { # stress_run <n> <duration>
  local n="$1" dur="$2" out name gname rc=0 statpid commit
  out="$HERE/results/$(date +%Y%m%d-%H%M%S)-$GEN$n"; mkdir -p "$out"
  name="jr-srv-$$-$n"; gname="jr-gen-$$-$n"
  # shellcheck disable=SC2064
  trap "podman rm -f $name $gname >/dev/null 2>&1 || true" EXIT
  box -d --name "$name" --sysctl net.ipv4.ip_local_port_range="1024 65535" \
    "$IMAGE" bash /src/submissions/julia-raw-amanica/dev/in-container.sh serve >/dev/null
  for _ in $(seq 600); do
    podman exec "$name" curl -sf http://127.0.0.1:3000/health >/dev/null 2>&1 && break
    podman container exists "$name" || { echo "server container died" >&2; return 1; }
    sleep 1
  done
  podman exec "$name" curl -sf http://127.0.0.1:3000/health >/dev/null || { podman logs "$name" | tail -30; return 1; }
  echo "== GEN=$GEN N=$n DURATION=$dur -> ${out#"$HERE"/}"
  sampler "$name" "$gname" > "$out/stats.log" 2>&1 &
  statpid=$!
  "run_$GEN" "$name" "$gname" "$n" "$dur" "$out" || rc=$?
  podman logs "$name" > "$out/server.log" 2>&1 || true
  podman rm -f "$name" >/dev/null 2>&1 || true
  wait "$statpid" 2>/dev/null || true
  trap - EXIT
  commit="$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo none)"
  git -C "$HERE" status --porcelain -- . 2>/dev/null | grep -q . && commit="$commit+dirty"
  RUN_PASS=0
  GEN="$GEN" LABEL="${LABEL//,/;}" NOTES="${NOTES//,/;}" CPUS="$CPUS" MEM="$MEM" VUS="$n" RAMP="$RAMP_UP" \
    DUR="$dur" COMMIT="$commit" python3 -I "$HERE/dev/record.py" "$out" "$rc" "$CSV" && RUN_PASS=1
  true
}

ladder() { # ladder <start> <max> <dur>
  local n="$1" max="$2" dur="$3" lo=0 hi=0 tol mid
  while [ "$n" -le "$max" ]; do
    stress_run "$n" "$dur"
    if [ "$RUN_PASS" = 1 ]; then lo="$n"; n=$((n * 2)); else hi="$n"; break; fi
  done
  if [ "$hi" = 0 ]; then echo "== passed up to $lo (MAX=$max reached)"; return; fi
  while :; do                                  # narrow down to within 5% (min 250 users / 8 connections)
    tol=$(( lo / 20 )); min=250; [ "$GEN" = blast ] && min=8; [ "$tol" -lt "$min" ] && tol="$min"
    [ $((hi - lo)) -le "$tol" ] && break
    mid=$(( (lo + hi) / 2 )); [ "$GEN" = k6 ] && mid=$(( mid / 50 * 50 ))
    { [ "$mid" -le "$lo" ] || [ "$mid" -ge "$hi" ]; } && break
    stress_run "$mid" "$dur"
    if [ "$RUN_PASS" = 1 ]; then lo="$mid"; else hi="$mid"; fi
  done
  echo "== highest passing: $lo (lowest failing: $hi)"
}

report() { python3 -I "$HERE/dev/report.py" "$CSV" "$HERE/README.md"; }

cmd="${1:-test}"; shift || true
if [ "$GEN" = blast ]; then def_n=256 def_start=16 def_max=16384 def_dur=30s
else def_n=1000 def_start=2500 def_max=320000 def_dur=1m; fi
case "$cmd" in
  image) image; echo "$IMAGE" ;;
  build|test) image; box "$IMAGE" bash /src/submissions/julia-raw-amanica/dev/in-container.sh "$cmd" ;;
  shell) image; box -it "$IMAGE" bash ;;
  stress) image; stress_run "${1:-$def_n}" "${2:-$def_dur}"; report >/dev/null; [ "$RUN_PASS" = 1 ] ;;
  ladder) image; ladder "${1:-$def_start}" "${2:-$def_max}" "${3:-$def_dur}"; report ;;
  report) report ;;
  *) sed -n '2,24p' "$0"; exit 2 ;;
esac
