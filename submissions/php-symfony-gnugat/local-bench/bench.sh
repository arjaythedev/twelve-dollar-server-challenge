#!/usr/bin/env bash
# Local stand-in for the $12 droplet: the server container is pinned to 1 core with 2 GB (no swap), so nproc, Go's
# GOMAXPROCS and FrankenPHP's defaults all see one CPU; k6 runs in its own container on the other cores. Numbers are only comparable with each other (Apple silicon, no CPU steal, no real network).
#
#   bash local-bench/bench.sh build              # build the image (runs install.sh and build.sh)
#   bash local-bench/bench.sh build-peer php-symfony-pklebba [git ref]
#                                                # build another submission (default ref origin/main), tagged
#                                                # twelve-dollar-<folder>: start it with IMAGE=twelve-dollar-<folder>
#   bash local-bench/bench.sh test               # what CI runs: test/run.sh, inside the image
#   bash local-bench/bench.sh start              # start the server (1 CPU, 2 GB) on a fresh database
#   bash local-bench/bench.sh load 2000 [label]  # k6 bench/load.js against it, CPU/RAM sampled to results/
#                                                # "hold CPU" = median server CPU during the hold: compare it at a
#                                                # fixed number of users to see small efficiency changes
#   bash local-bench/bench.sh stop
#
# Env: DURATION (default 5m), RAMP_UP (default 60s), the same as bench/load.js and the real score; shorter for quick runs.
#      WARMUP=0 skips the unmeasured 1,000-user, 2-minute warm-up that load runs first, like the real score.
#      NOFILE: the server's open-files limit (default 65535, as the rules give it: one per keep-alive connection)
#      K6_CONTAINERS: k6 containers sharing the users (default 1, 2 above 60,000 users: one runs out of ports near 64,000)
#      SERVE_ARGS: extra `docker run` args for start, e.g. to try another Caddyfile:
#      SERVE_ARGS="-v $PWD/local-bench/Caddyfile.classic:/app/Caddyfile:ro" bash local-bench/bench.sh start
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$APP/../.." && pwd)"
IMAGE="${IMAGE:-twelve-dollar-php-symfony}"
NET=twelve-dollar
SERVER=twelve-dollar-server
K6=twelve-dollar-k6
RESULTS="$HERE/results"
SERVER_CPU=1                                         # core 0 is left to the VM's interrupts
K6_CPUS="2-$(( $(docker info --format '{{.NCPU}}') - 1 ))"

case "${1:-}" in
  build)
    docker build -f "$HERE/Dockerfile" -t "$IMAGE" "$APP"
    ;;
  build-peer)
    peer="${2:?usage: bench.sh build-peer <submission folder> [git ref]}"; ref="${3:-origin/main}"
    src="$(mktemp -d)"
    git -C "$REPO" archive "$ref" "submissions/$peer" | tar -x -C "$src"
    docker build -f "$HERE/Dockerfile.peer" --build-context bench="$HERE" -t "twelve-dollar-$peer" "$src/submissions/$peer"
    rm -rf "$src"
    ;;
  test)
    docker run --rm --cpuset-cpus="$SERVER_CPU" --memory=2g --memory-swap=2g \
      -v "$REPO/test:/repo/test:ro" -v "$REPO/seed:/repo/seed:ro" \
      "$IMAGE" bash /repo/test/run.sh /app
    ;;
  start)
    docker network inspect "$NET" >/dev/null 2>&1 || docker network create "$NET" >/dev/null
    docker rm -f "$SERVER" >/dev/null 2>&1 || true
    docker run -d --name "$SERVER" --network "$NET" -p 3000:80 \
      --cpuset-cpus="$SERVER_CPU" --memory=2g --memory-swap=2g --ulimit nofile="${NOFILE:-65535}:${NOFILE:-65535}" \
      -v "$REPO/seed:/seed:ro" ${SERVE_ARGS:-} "$IMAGE" >/dev/null
    for _ in $(seq 60); do curl -sf http://127.0.0.1:3000/health >/dev/null && break; sleep 1; done
    curl -sf http://127.0.0.1:3000/health && echo
    ;;
  load)
    vus="${2:?usage: bench.sh load <vus> [label]}"; label="${3:-run}"
    mkdir -p "$RESULTS"; out="$RESULTS/$(date +%Y%m%d-%H%M%S)-$label-$vus"
    # like the real score: 1,000 users for 2 minutes first, not measured (WARMUP=0 to skip)
    if [ "${WARMUP:-1}" != 0 ]; then
      docker run --rm --name "$K6" --network "$NET" --cpuset-cpus="$K6_CPUS" --ulimit nofile=200000:200000 \
        -v "$REPO/bench:/bench:ro" -v "$REPO/seed:/seed:ro" \
        -e BASE_URL="http://$SERVER" -e VUS=1000 -e TOKENS=/seed/tokens.json -e DURATION=2m \
        grafana/k6:1.4.0 run --quiet /bench/load.js > "$out.warmup" 2>&1 || true
    fi
    docker stats --format '{{.CPUPerc}} {{.MemUsage}}' "$SERVER" > "$out.stats" &
    stats_pid=$!
    # one k6 container runs out of ports to the server near 64,000 users (one source IP): above 60,000 users the load
    # is split across two containers, each with its own IP (K6_CONTAINERS overrides it)
    k6s="${K6_CONTAINERS:-$([ "$vus" -gt 60000 ] && echo 2 || echo 1)}"; k6_names="$K6"; [ "$k6s" = 1 ] || k6_names="$(seq -f "$K6-%g" "$k6s")"
    # and k6's, to tell a saturated server from a saturated load generator
    ( for name in $k6_names; do until docker inspect "$name" >/dev/null 2>&1; do sleep 1; done; done
      exec docker stats --format '{{.CPUPerc}} {{.MemUsage}}' $k6_names ) > "$out.k6stats" 2>/dev/null &
    k6_stats_pid=$!
    rc=0
    # k6 (not the server) gets more ephemeral ports: the default ~28k caps one load generator at ~28k users
    k6() {
      docker run --rm --name "$1" --network "$NET" --cpuset-cpus="$K6_CPUS" --ulimit nofile=200000:200000 --sysctl net.ipv4.ip_local_port_range="1024 65535" \
        -v "$REPO/bench:/bench:ro" -v "$REPO/seed:/seed:ro" \
        -e BASE_URL="http://$SERVER" -e VUS="$2" -e TOKENS=/seed/tokens.json \
        -e DURATION="${DURATION:-5m}" -e RAMP_UP="${RAMP_UP:-60s}" \
        grafana/k6:1.4.0 run --quiet /bench/load.js 2>&1
    }
    if [ "$k6s" = 1 ]; then
      k6 "$K6" "$vus" | tee "$out.k6" || rc=$?
    else
      pids=""
      for name in $k6_names; do k6 "$name" $(( vus / k6s )) > "$out.k6.${name##*-}" & pids="$pids $!"; done
      for pid in $pids; do wait "$pid" || rc=$?; done
      cat "$out".k6.*
    fi
    kill "$stats_pid" "$k6_stats_pid" 2>/dev/null || true
    # docker stats redraws with ANSI codes: keep the numbers, report peaks
    sed 's/\x1b\[[0-9;]*[A-Za-z]//g' "$out.stats" | grep -Eo '[0-9.]+% [0-9.]+[KMG]iB' > "$out.samples" || true
    echo "peak CPU: $(cut -d' ' -f1 "$out.samples" | tr -d % | sort -n | tail -1)%  peak RAM: $(cut -d' ' -f2 "$out.samples" | sort -h | tail -1)"
    # samples are evenly spaced over ramp-up + hold + 30 s ramp-down: keep the middle 60% of the hold
    secs() { case "$1" in *m) echo $(( ${1%m} * 60 ));; *s) echo "${1%s}";; esac; }
    ramp=$(secs "${RAMP_UP:-60s}"); hold=$(secs "${DURATION:-5m}"); n=$(wc -l < "$out.samples")
    from=$(( n * (ramp + hold / 5) / (ramp + hold + 30) + 1 )); to=$(( n * (ramp + hold * 4 / 5) / (ramp + hold + 30) ))
    hold_cpu="$(sed -n "${from},${to}p" "$out.samples" | cut -d' ' -f1 | tr -d % | sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}')%"
    echo "hold CPU (median): $hold_cpu"
    # one row per run in results/summary.tsv: requests = total served, the efficiency measure once the server saturates
    if [ "$k6s" = 1 ]; then
      duration_line="$(grep -E 'http_req_duration\.' "$out.k6")"; failed_line="$(grep -E 'http_req_failed\.' "$out.k6")"
      p95="$(grep -Eo 'p\(95\)=[^ ]+' <<< "$duration_line" | cut -d= -f2)"; p99="$(grep -Eo 'p\(99\)=[^ ]+' <<< "$duration_line" | cut -d= -f2)"
      errors="$(awk '{print $2}' <<< "$failed_line")"; requests="$(awk '{print $NF}' <<< "$failed_line")"
    else
      # each container sees the same load: the worse percentile of the two stands for both; requests and errors add up
      worst() { grep -hE 'http_req_duration\.' "$out".k6.* | grep -Eo "p\\($1\\)=[^ ]+" | cut -d= -f2 \
        | awk '{v=$1; ms=v+0; if (v ~ /µs$/) ms/=1000; else if (v !~ /ms$/) ms*=1000; if (ms >= max) {max=ms; worst=v}} END {print worst}'; }
      p95="$(worst 95)"; p99="$(worst 99)"
      requests="$(grep -hE 'http_req_failed\.' "$out".k6.* | awk '{n+=$NF} END {print n}')"
      errors="$(grep -hE 'http_req_failed\.' "$out".k6.* | awk '{f+=$3; n+=$NF} END {printf "%.2f%%", 100*f/n}')"
    fi
    [ -f "$RESULTS/summary.tsv" ] || printf 'time\tlabel\tusers\tp95\tp99\terrors\trequests\thold_cpu\tpeak_ram\tresult\n' > "$RESULTS/summary.tsv"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date +%H:%M)" "$label" "$vus" "$p95" "$p99" "$errors" "$requests" "$hold_cpu" \
      "$(cut -d' ' -f2 "$out.samples" | sort -h | tail -1)" "$([ "$rc" = 0 ] && echo pass || echo FAIL)" >> "$RESULTS/summary.tsv"
    sed 's/\x1b\[[0-9;]*[A-Za-z]//g' "$out.k6stats" | grep -Eo '[0-9.]+% [0-9.]+[KMG]iB' > "$out.k6samples" || true
    echo "k6 peak CPU: $(cut -d' ' -f1 "$out.k6samples" | tr -d % | sort -n | tail -1)% (of $(( $(docker info --format '{{.NCPU}}') - 2 )) cores)  k6 peak RAM: $(cut -d' ' -f2 "$out.k6samples" | sort -h | tail -1)"
    echo "k6 exit code: $rc (0 = thresholds passed)  -> $out.k6"
    ;;
  stop)
    docker rm -f "$SERVER" >/dev/null 2>&1 || true
    ;;
  *)
    sed -n '2,14p' "$0"; exit 1
    ;;
esac
