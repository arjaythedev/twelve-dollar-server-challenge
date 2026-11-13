#!/usr/bin/env bash
# Build, test and stress test the submission in podman containers that mimic the box:
# Ubuntu 24.04, 1 CPU, 2 GB RAM, no swap, nofile 65535. See README.md "Running locally".
#
#   dev/ct.sh image                       build the dev images
#   dev/ct.sh build                       run build.sh in a clean container
#   dev/ct.sh test                        test/run.sh (build + start + 42 checks), as CI does
#   dev/ct.sh feedbench                   time GET /feed query variants in-process (dev/feedbench.jl)
#   dev/ct.sh stress [N] [DUR]            one run on a fresh server + DB; appends a row to stress-results.csv
#   dev/ct.sh ladder [START] [MAX] [DUR]  double N until a run fails, then bisect to within 5%
#   dev/ct.sh report                      rewrite the results section of README.md from the CSV
#   dev/ct.sh shell                       interactive shell in the dev image
#   dev/ct.sh clean                       remove leftover stress containers (jr-srv-*, jr-gen-*)
#
# GEN picks the load generator:
#   GEN=k6     (default) bench/load.js, the real user model. N = virtual users. Costs ~0.3 MB per user.
#   GEN=blast  wrk with the same request mix and no think time. N = open connections. Cheap; measures
#              max req/s (users estimated as req/s x 10.6).
#   GEN=users  wrk with the same mix plus think time (3-16 s) per connection: N connections ~ N users.
#              ~22 KB per user, so 100k+ users fit; shows connection limits (fds, memory).
# --debug (first argument): also count the server's connections (fds) and log its allocation/GC stats
#   (JR_DEBUG=1). Off by default so measuring costs nothing; the CSV's debug column says which.
# Env: CKPT (auto: SQLite's automatic checkpoint on the event loop, for A/B; default: background thread),
#      HOLD_PAGES (WAL pages at which writes wait for the checkpoint; 0: never; default in src/db.jl),
#      RESTART (loop: the event loop's next write restarts the WAL, for A/B; default: the checkpoint thread),
#      GROUP (off: one commit per write, for A/B; default: one commit per event-loop pass),
#      FEED (plain: GET /feed as one query, for A/B; default: range scans when the newest ids are contiguous),
#      PROFILE ("<start s>,<seconds>": sample the event loop then, table in server.log; src/prof.jl),
#      DATA (volume: the DB copy on a per-run podman volume on the host's disk; overlay: on the container's
#      fuse-overlayfs, as before 2026-11-08, which blocks the loop on every write),
#      LABEL (free text for the CSV row), NOTES, CPUS (1), MEM (2g), RAMP_UP (30s, k6 only),
#      WARMUP (5s, blast only), WRK_THREADS (4, per wrk process), SHARDS (parallel wrk processes;
#      default N / CONNS_PER_SHARD), CONNS_PER_SHARD (5000), SECCOMP (unconfined; "default" to test without io_uring).
# Raw output of each run: results/<timestamp>-<gen><N>/ (gitignored). The CSV is committed.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CSV="$HERE/stress-results.csv"
IMAGE=localhost/julia-iouring-amanica-dev
WRK_IMAGE=localhost/julia-iouring-amanica-wrk
K6_IMAGE="${K6_IMAGE:-docker.io/grafana/k6:2.3.0@sha256:2dd4756ec6e14e62778916e1eb850108e4b499304c02df56760fa0bdb0fba0b8}"
DATA="${DATA:-volume}"   # where the server's DB copy lives: volume (host ext4) or overlay (fuse-overlayfs)
GEN="${GEN:-k6}" CPUS="${CPUS:-1}" MEM="${MEM:-2g}" RAMP_UP="${RAMP_UP:-30s}" WARMUP="${WARMUP:-5s}"
WRK_THREADS="${WRK_THREADS:-4}" LABEL="${LABEL:-dev}" NOTES="${NOTES:-}"
case "$GEN" in k6|blast|users) ;; *) echo "GEN must be k6, blast or users" >&2; exit 2 ;; esac
SEC=(); [ "${SECCOMP:-unconfined}" = unconfined ] && SEC=(--security-opt seccomp=unconfined)
[ -f "$ROOT/seed/feed.db" ] || { echo "no seed/feed.db: run bash seed/make-seed.sh in $ROOT" >&2; exit 1; }

# dev/tools.jl runs on the host in its own project environment (dev/Project.toml + Manifest.toml, pinned).
# JULIA_DEV picks the julia binary: juliaup's 1.13 channel if there is one, else julia on PATH.
JULIA_DEV="${JULIA_DEV:-$(command -v juliaup >/dev/null && juliaup status 2>/dev/null | grep -q ' 1\.13 ' && echo 'julia +1.13' || echo julia)}"
tools() {
  local stamp="$HERE/results/.dev-env" want
  want="$(sha256sum "$HERE/dev/Manifest.toml" | cut -c1-16)"
  mkdir -p "$HERE/results"
  if [ "$(cat "$stamp" 2>/dev/null)" != "$want" ]; then
    $JULIA_DEV --startup-file=no --project="$HERE/dev" -e 'using Pkg; Pkg.instantiate()' >&2 && echo "$want" > "$stamp"
  fi
  $JULIA_DEV --startup-file=no --project="$HERE/dev" "$HERE/dev/tools.jl" "$@"
}

image() {
  podman build -q -f "$HERE/dev/Containerfile" -t "$IMAGE" "$HERE" >/dev/null
  [ "$GEN" != k6 ] && podman build -q -f "$HERE/dev/Containerfile.wrk" -t "$WRK_IMAGE" "$HERE/dev" >/dev/null
  true
}
BOX_RM=(--rm)
box() {
  podman run "${BOX_RM[@]}" --cpus "$CPUS" --memory "$MEM" --memory-swap "$MEM" --ulimit nofile=65535:65535 \
    "${SEC[@]}" -v "$ROOT:/src:ro" -v julia-iouring-amanica-dl:/cache:U -e JR_DL_CACHE=/cache "$@"
}

# Samples every 2 s while the server container lives, from the HOST side (cgroup files and /proc of the
# container's pid 1, which is julia after the execs), so sampling costs the server's 1 CPU nothing:
# CPU % over the window (cgroup usage_usec deltas), the event loop thread's on-CPU % (main) and run-queue
# wait % (runq: runnable but not running, i.e. the host starving it), cgroup memory, julia RSS,
# the generator container's memory and, with --debug, julia's open fds (~ connections + ~23).
sampler() { # sampler <server> <generator>
  set +e +o pipefail   # a failed read must not end the sampling
  local srv="$1" g="$2" pid cg prev="" usec thr now_ns mem rss fds gm run wait stime TICKS
  TICKS="$(getconf CLK_TCK)"
  pid="$(podman inspect -f '{{.State.Pid}}' "$srv" 2>/dev/null)" || return 0
  cg="/sys/fs/cgroup$(cut -d: -f3- "/proc/$pid/cgroup" 2>/dev/null)"
  while [ -d "/proc/$pid" ] && podman container exists "$srv"; do
    usec="$(awk '/usage_usec/{print $2}' "$cg/cpu.stat" 2>/dev/null)"; now_ns="$(date +%s%N)"
    thr="$(awk '/throttled_usec/{print $2}' "$cg/cpu.stat" 2>/dev/null)"
    # main thread (the event loop): ns on a CPU, ns runnable but waiting for one (run-queue delay)
    read -r run wait _ < "/proc/$pid/schedstat" 2>/dev/null || { run=0; wait=0; }
    # main thread's kernel time in clock ticks (stime, field 15; "julia" has no spaces, so fields are plain)
    stime="$(awk '{print $15}' "/proc/$pid/task/$pid/stat" 2>/dev/null)"; stime="${stime:-0}"
    if [ -n "$prev" ] && [ -n "$usec" ]; then
      set -- $prev     # prev_usec prev_ns prev_run prev_wait prev_thr prev_stime
      mem="$(cat "$cg/memory.current" 2>/dev/null || echo 0)"
      rss="$(awk '/^VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null)"
      fds=""   # /proc/<pid>/fd of a rootless container is only readable inside podman's user namespace
      [ "$DEBUG" = 1 ] && fds=" fds=$(podman unshare ls "/proc/$pid/fd" 2>/dev/null | wc -l || true)"
      gm="$(podman stats --no-stream --format '{{.MemUsage}}' "$g" 2>/dev/null | cut -d' ' -f1)"
      echo "$(date +%T) cpu=$(( (usec - $1) * 100000 / (now_ns - $2) )) main=$(( (run - $3) * 100 / (now_ns - $2) )) sys=$(( (stime - $6) * 100 * 1000000000 / TICKS / (now_ns - $2) )) runq=$(( (wait - $4) * 100 / (now_ns - $2) )) thr=$(( (${thr:-0} - $5) * 100000 / (now_ns - $2) )) cgmem_mb=$(( mem >> 20 )) rss_mb=$(( ${rss:-0} >> 10 ))$fds genmem=${gm:-?}"
    fi
    [ -n "$usec" ] && prev="$usec $now_ns $run $wait ${thr:-0} $stime"
    sleep 2
  done
}

secs() { case "$1" in *m) echo $(( ${1%m} * 60 ));; *s) echo "${1%s}";; *) echo "$1";; esac; }

# Runs N connections as parallel wrk processes ("shards") inside one generator container. Each shard
# targets its own 127.0.0.x (the server binds 0.0.0.0), so no (src, dst) pair runs out of ephemeral ports.
# users: shard start times are spread over RAMP_UP (wrk opens all its connections at once), and all
# shards end together. Output: <out>/shard-<i>.txt, merged by record.py.
run_wrk() { # run_wrk <server> <name> <n> <dur> <out> <lua> <stagger 0|1>
  local n="$3" per shards t dur_s ramp_s script i c off len
  per="${CONNS_PER_SHARD:-5000}"
  shards="${SHARDS:-$(( (n + per - 1) / per ))}"; [ "$shards" -lt 1 ] && shards=1
  per=$(( (n + shards - 1) / shards ))
  t="$WRK_THREADS"; [ "$per" -lt "$t" ] && t="$per"
  dur_s="$(secs "$4")"; ramp_s=0; [ "$7" = 1 ] && ramp_s="$(secs "$RAMP_UP")"
  script=""
  for ((i = 0; i < shards; i++)); do
    c=$(( i < shards - 1 ? per : n - per * (shards - 1) )); [ "$c" -lt 1 ] && break
    off="$(awk "BEGIN{printf \"%.2f\", $ramp_s * $i / $shards}")"
    len="$(awk "BEGIN{printf \"%d\", $ramp_s - $off + $dur_s}")"
    script+="( sleep $off; exec wrk -t$(( c < t ? c : t )) -c$c -d${len}s --timeout 10s -s $6 http://127.0.0.$(( i % 250 + 1 )):3000 > /out/shard-$i.txt 2>&1 ) & "
  done
  script+="wait"
  echo "   $shards shard(s) x ~$per connections, $t thread(s) each$([ "$7" = 1 ] && echo ", started over $RAMP_UP")"
  podman run --rm --name "$2" --network "container:$1" --ulimit nofile="$(ulimit -Hn)":"$(ulimit -Hn)" \
    -v "$HERE/dev/blast.lua:/blast.lua:ro" -v "$HERE/dev/users.lua:/users.lua:ro" \
    -v "$ROOT/seed/tokens.json:/seed/tokens.json:ro" -v "$5:/out" --entrypoint sh "$WRK_IMAGE" -c "$script"
}

run_blast() { # warm-up (JIT, page cache) first, not recorded
  mkdir -p "$5/warmup"
  local d="$4"; set -- "$1" "$2" "$3" "$WARMUP" "$5/warmup"; run_wrk "$@" /blast.lua 0 >/dev/null
  set -- "$1" "$2" "$3" "$d" "${5%/warmup}"; run_wrk "$@" /blast.lua 0
}
run_users() { run_wrk "$@" /users.lua 1; }

# One run. Sets RUN_PASS=1/0.
stress_run() { # stress_run <n> <duration>
  local n="$1" dur="$2" out name gname rc=0 statpid commit
  out="$HERE/results/$(date +%Y%m%d-%H%M%S)-$GEN$n"; mkdir -p "$out"
  name="jr-srv-$$-$n"; gname="jr-gen-$$-$n"
  # shellcheck disable=SC2064
  # remove this run's containers however the script ends (an interrupted run left idle servers behind)
  local vol="jr-data-$$-$n" dataopt=(-e JR_DATA=/work/data)
  [ "$DATA" = volume ] && dataopt=(-v "$vol:/data:U" -e JR_DATA=/data)
  trap "podman rm -f $name $gname >/dev/null 2>&1; podman volume rm -f $vol >/dev/null 2>&1 || true" EXIT
  trap "podman rm -f $name $gname >/dev/null 2>&1; podman volume rm -f $vol >/dev/null 2>&1; exit 130" INT TERM HUP PIPE
  BOX_RM=()    # keep the server container until we have its log, also when it dies at startup
  box -d --name "$name" "${dataopt[@]}" -e JR_DEBUG="$DEBUG" -e JR_TRANSPORT="${TRANSPORT:-}" -e JR_CKPT="${CKPT:-}" -e JR_HOLD_PAGES="${HOLD_PAGES:-}" -e JR_RESTART="${RESTART:-}" -e JR_GROUP="${GROUP:-}" -e JR_PROFILE="${PROFILE:-}" -e JR_FEED="${FEED:-}" --sysctl net.ipv4.ip_local_port_range="1024 65535" \
    "$IMAGE" bash /src/submissions/julia-iouring-amanica/dev/in-container.sh serve >/dev/null
  for _ in $(seq 600); do
    podman exec "$name" curl -sf http://127.0.0.1:3000/health >/dev/null 2>&1 && break
    [ "$(podman inspect -f '{{.State.Running}}' "$name" 2>/dev/null)" = true ] || break
    sleep 1
  done
  if ! podman exec "$name" curl -sf http://127.0.0.1:3000/health >/dev/null 2>&1; then
    podman logs "$name" > "$out/server.log" 2>&1 || true
    echo "FAIL: server not healthy (log: ${out#"$HERE"/}/server.log):" >&2; tail -20 "$out/server.log" >&2
    return 1
  fi
  echo "== GEN=$GEN N=$n DURATION=$dur -> ${out#"$HERE"/}"
  sampler "$name" "$gname" > "$out/stats.log" 2>&1 &
  statpid=$!
  "run_$GEN" "$name" "$gname" "$n" "$dur" "$out" || rc=$?
  podman logs "$name" > "$out/server.log" 2>&1 || true
  podman rm -f "$name" >/dev/null 2>&1 || true
  podman volume rm -f "$vol" >/dev/null 2>&1 || true
  wait "$statpid" 2>/dev/null || true
  trap - EXIT INT TERM HUP PIPE
  commit="$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo none)"
  git -C "$HERE" status --porcelain -- . 2>/dev/null | grep -q . && commit="$commit+dirty"
  RUN_PASS=0
  local notes="${NOTES:+$NOTES; }DATA=$DATA${CKPT:+; CKPT=$CKPT}${HOLD_PAGES:+; HOLD_PAGES=$HOLD_PAGES}${RESTART:+; RESTART=$RESTART}${GROUP:+; GROUP=$GROUP}${PROFILE:+; PROFILE=$PROFILE}${FEED:+; FEED=$FEED}"
  DEBUG="$DEBUG" GEN="$GEN" LABEL="${LABEL//,/;}" NOTES="${notes//,/;}" CPUS="$CPUS" MEM="$MEM" VUS="$n" RAMP="$RAMP_UP" \
    DUR="$dur" COMMIT="$commit" tools record "$out" "$rc" "$CSV" && RUN_PASS=1
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

report() { tools report "$CSV" "$HERE/README.md"; }

DEBUG="${DEBUG:-0}"
[ "${1:-}" = --debug ] && { DEBUG=1; shift; }
# Stress runs share the host with their load generator: two at once distort each other's numbers.
only_run() {
  local other
  other="$(podman ps --format '{{.Names}}' | grep -E '^jr-(srv|gen)-' | head -1 || true)"
  [ -z "$other" ] || { echo "another stress run is active (container $other): wait for it, or dev/ct.sh clean" >&2; exit 3; }
}

cmd="${1:-test}"; shift || true
if [ "$GEN" = blast ]; then def_n=256 def_start=16 def_max=16384 def_dur=30s
elif [ "$GEN" = users ]; then def_n=20000 def_start=10000 def_max=320000 def_dur=1m
else def_n=1000 def_start=2500 def_max=320000 def_dur=1m; fi
case "$cmd" in
  image) image; echo "$IMAGE" ;;
  build|test|feedbench) image; box "$IMAGE" bash /src/submissions/julia-iouring-amanica/dev/in-container.sh "$cmd" ;;
  shell) image; box -it "$IMAGE" bash ;;
  stress) only_run; image; stress_run "${1:-$def_n}" "${2:-$def_dur}"; report >/dev/null; [ "$RUN_PASS" = 1 ] ;;
  ladder) only_run; image; ladder "${1:-$def_start}" "${2:-$def_max}" "${3:-$def_dur}"; report ;;
  report) report ;;
  clean) n="$(podman ps -a --format '{{.Names}}' | grep -E '^jr-(srv|gen)-' || true)"
         # generators first: they share their server's network namespace, so podman refuses the server before them
         for c in $(echo "$n" | grep '^jr-gen-') $(echo "$n" | grep '^jr-srv-'); do podman rm -f "$c" >/dev/null; done
         v="$(podman volume ls --format '{{.Name}}' | grep '^jr-data-' || true)"
         [ -n "$v" ] && podman volume rm -f $v >/dev/null; echo "removed: ${n:-no containers} ${v:-no volumes}" ;;
  *) sed -n '2,30p' "$0"; exit 2 ;;
esac
