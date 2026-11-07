#!/usr/bin/env bash
# Build, test and stress test the submission in a podman container that mimics the box:
# Ubuntu 24.04, 1 CPU, 2 GB RAM, no swap, nofile 65535.
#
#   dev/ct.sh image                 build the dev image (install.sh baked in)
#   dev/ct.sh build                 run build.sh in a clean container
#   dev/ct.sh test                  test/run.sh (build + start + 42 checks), as CI does
#   dev/ct.sh stress [VUS] [DUR]    server container + k6 container sharing its network namespace
#   dev/ct.sh shell                 interactive shell in the dev image
#
# Env: CPUS (1), MEM (2g), RAMP_UP (60s), SECCOMP (unconfined; "default" to test without io_uring),
#      K6_IMAGE. Results of stress runs go to results/<timestamp>/ (gitignored).
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
IMAGE=localhost/julia-raw-amanica-dev
K6_IMAGE="${K6_IMAGE:-docker.io/grafana/k6:2.3.0@sha256:2dd4756ec6e14e62778916e1eb850108e4b499304c02df56760fa0bdb0fba0b8}"
CPUS="${CPUS:-1}" MEM="${MEM:-2g}"
SEC=(); [ "${SECCOMP:-unconfined}" = unconfined ] && SEC=(--security-opt seccomp=unconfined)
[ -f "$ROOT/seed/feed.db" ] || { echo "no seed/feed.db: run bash seed/make-seed.sh in $ROOT" >&2; exit 1; }

image() { podman build -q -f "$HERE/dev/Containerfile" -t "$IMAGE" "$HERE" >/dev/null; }
box() { # box <podman run args...> -- <in-container command>
  podman run --rm --cpus "$CPUS" --memory "$MEM" --memory-swap "$MEM" --ulimit nofile=65535:65535 \
    "${SEC[@]}" -v "$ROOT:/src:ro" -v julia-raw-amanica-dl:/cache:U -e JR_DL_CACHE=/cache "$@"
}

cmd="${1:-test}"; shift || true
case "$cmd" in
  image) image; echo "$IMAGE" ;;
  build|test) image; box "$IMAGE" bash /src/submissions/julia-raw-amanica/dev/in-container.sh "$cmd" ;;
  shell) image; box -it "$IMAGE" bash ;;
  stress)
    VUS="${1:-1000}" DUR="${2:-2m}"
    image
    out="$HERE/results/$(date +%Y%m%d-%H%M%S)-vus$VUS"; mkdir -p "$out"
    name="jr-stress-$$"
    trap 'podman rm -f "$name" >/dev/null 2>&1 || true' EXIT
    box -d --name "$name" "$IMAGE" bash /src/submissions/julia-raw-amanica/dev/in-container.sh serve >/dev/null
    echo "waiting for /health (build + start)..."
    for _ in $(seq 600); do
      podman exec "$name" curl -sf http://127.0.0.1:3000/health >/dev/null 2>&1 && break
      podman container exists "$name" || { echo "server container died"; exit 1; }
      sleep 1
    done
    podman exec "$name" curl -sf http://127.0.0.1:3000/health || { podman logs "$name" | tail -30; exit 1; }
    echo; echo "k6: VUS=$VUS DURATION=$DUR -> $out"
    # CPU % over each 5 s window (cgroup usage_usec deltas; podman stats averages since start) and RSS.
    ( prev=""; while podman container exists "$name"; do
        cur="$(podman exec "$name" sh -c 'awk "/usage_usec/{print \$2}" /sys/fs/cgroup/cpu.stat; cat /sys/fs/cgroup/memory.current /sys/fs/cgroup/memory.peak 2>/dev/null' 2>/dev/null | tr '\n' ' ')"
        set -- $cur; now_ns="$(date +%s%N)"
        if [ -n "$prev" ] && [ -n "${1:-}" ]; then
          set -- $prev "$@"   # prev_usec prev_ns usec mem peak
          echo "$(date +%T) cpu=$(( ($3 - $1) * 100000 / (now_ns - $2) ))% mem=$(( $4 >> 20 ))MB peak=$(( ${5:-0} >> 20 ))MB"
          prev="$3 $now_ns"
        else
          prev="${1:-} $now_ns"
        fi
        sleep 5
      done ) > "$out/stats.log" 2>&1 &
    statpid=$!
    rc=0
    podman run --rm --user 0 --network "container:$name" --ulimit nofile="$(ulimit -Hn)":"$(ulimit -Hn)" \
      -v "$ROOT/bench:/bench:ro" -v "$ROOT/seed/tokens.json:/seed/tokens.json:ro" -v "$out:/out" \
      "$K6_IMAGE" run -q -e VUS="$VUS" -e DURATION="$DUR" -e RAMP_UP="${RAMP_UP:-60s}" \
      -e TOKENS=/seed/tokens.json --summary-export /out/summary.json /bench/load.js | tee "$out/k6.txt" || rc=$?
    kill "$statpid" 2>/dev/null || true
    podman logs "$name" > "$out/server.log" 2>&1 || true
    sed 's/cpu=//' "$out/stats.log" | sort -k2 -n | tail -1 | sed 's/^/busiest 5 s window: cpu=/'
    echo "k6 exit $rc (0 = thresholds passed)"; exit $rc ;;
  *) sed -n '2,15p' "$0"; exit 2 ;;
esac
