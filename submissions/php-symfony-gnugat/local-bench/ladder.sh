#!/usr/bin/env bash
# The ladder: every phase of this submission, in worker and classic mode, throttled to a quarter of the core
# (0.25 CPU) and saturated, so that the requests served per second measure each phase's capacity.
#
#   bash local-bench/ladder.sh build            # one image per phase commit (twelve-dollar-php-symfony:ladder-pN)
#   bash local-bench/ladder.sh run <rounds>     # every setup once per round, results/ladder.tsv
#
# The measure only holds between saturation and timeouts: under capacity, the server serves what it's offered;
# far above it (several times), queued requests reach k6's 60 s timeout, and the server's CPU goes to requests
# nobody waits for. So each setup's load calibrates itself: errors above 1% halve it, p95 under 1 s doubles it,
# and the run starts again; the next rounds start from the calibrated load. Users only wait longer at higher loads:
# within the window, requests served per second stay the same.
# Env: PHASES (default: every phase commit of the branch, from git log), BASE (the commit before phase 0, default: the
#      merge base with origin/main).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$(cd "$HERE/.." && pwd)"
REPO="$(git -C "$APP" rev-parse --show-toplevel)"
PREFIX="$(git -C "$APP" rev-parse --show-prefix)"
RESULTS="$HERE/results"
STATE="$RESULTS/ladder"
BASE="${BASE:-$(git -C "$APP" merge-base HEAD origin/main)}"
mkdir -p "$STATE"

# "<n> <hash>" for each phase commit: "Add php-symfony-gnugat" is phase 0, then "phase N," subjects
phases() {
  git -C "$REPO" log --reverse --format='%h %s' "$BASE..HEAD" -- "$PREFIX" | while read -r hash subject; do
    case "$subject" in
      Add\ php-symfony-gnugat*) echo "0 $hash" ;;
      *phase\ [0-9]*,*) echo "$(sed -E 's/.*phase ([0-9]+),.*/\1/' <<< "$subject") $hash" ;;
    esac
  done
}

case "${1:-}" in
  build)
    while read -r n hash <&3; do
      src="$(mktemp -d)"
      git -C "$REPO" archive "$hash" "$PREFIX" | tar -x -C "$src"
      # phase 0 comes before local-bench/: build it with this checkout's
      [ -d "$src/$PREFIX/local-bench" ] || cp -R "$HERE" "$src/$PREFIX/local-bench"
      cp "$src/$PREFIX/local-bench/Caddyfile.classic" "$STATE/Caddyfile.classic.p$n"
      IMAGE="twelve-dollar-php-symfony:ladder-p$n" bash "$src/$PREFIX/local-bench/bench.sh" build > "$STATE/build-p$n.log" 2>&1
      echo "phase $n ($hash): build exit $?, $(IMAGE="twelve-dollar-php-symfony:ladder-p$n" bash "$HERE/bench.sh" test 2>&1 | tail -1)"
      rm -rf "$src"
    done 3< <(phases)
    ;;
  run)
    rounds="${2:?usage: ladder.sh run <rounds>}"
    [ -f "$RESULTS/ladder.tsv" ] || printf 'time\tphase\tmode\tusers\tp95\tp99\terrors\tserved\tserved_per_s\tcpu\tpeak_ram\n' > "$RESULTS/ladder.tsv"
    secs() { awk '{v=$1; if (v ~ /ms$/) {sub(/ms$/,"",v); print v/1000} else if (v ~ /µs$/) {sub(/µs$/,"",v); print v/1e6} else if (v ~ /m[0-9.]+s$/) {split(v,a,"m"); sub(/s$/,"",a[2]); print a[1]*60+a[2]} else {sub(/s$/,"",v); print v+0}}' <<< "$1"; }
    for _ in $(seq "$rounds"); do
      while read -r n _hash <&3; do
        for mode in worker classic; do
          users_file="$STATE/users.p$n.$mode"
          [ -f "$users_file" ] || { if [ "$mode" = worker ]; then [ "$n" -ge 6 ] && echo 10000 || echo 3000; else [ "$n" -ge 9 ] && echo 4000 || echo 2000; fi; } > "$users_file"
          args="--cpus=0.25"; [ "$mode" = classic ] && args="$args -v $STATE/Caddyfile.classic.p$n:/app/Caddyfile:ro"
          for _try in 1 2 3 4 5; do
            users="$(cat "$users_file")"
            IMAGE="twelve-dollar-php-symfony:ladder-p$n" SERVE_ARGS="$args" bash "$HERE/bench.sh" start >/dev/null
            out="$(WARMUP=0 DURATION=2m RAMP_UP=30s bash "$HERE/bench.sh" load "$users" "ladder-p$n-$mode" 2>&1)"
            p95="$(grep -E 'http_req_duration\.' <<< "$out" | grep -Eo 'p\(95\)=[^ ]+' | cut -d= -f2)"
            p99="$(grep -E 'http_req_duration\.' <<< "$out" | grep -Eo 'p\(99\)=[^ ]+' | cut -d= -f2)"
            failed_line="$(grep -E 'http_req_failed\.' <<< "$out")"
            errors="$(awk '{print $2}' <<< "$failed_line")"; failed="$(awk '{print $3}' <<< "$failed_line")"; total="$(awk '{print $NF}' <<< "$failed_line")"
            cpu="$(grep -Eo 'hold CPU \(median\): [0-9.]+%' <<< "$out" | grep -Eo '[0-9.]+%')"; ram="$(grep -Eo '^peak CPU: .*peak RAM: [0-9.]+[KMG]iB' <<< "$out" | grep -Eo 'peak RAM: [0-9.]+[KMG]iB' | awk '{print $3}')"
            if [ -z "$total" ] || [ -z "$p95" ]; then echo "phase $n $mode: no k6 results, see results/*-ladder-p$n-$mode-$users.k6" >&2; exit 1; fi
            served=$(( total - failed ))
            if awk "BEGIN {exit !(${errors%\%} > 1)}"; then echo $(( users / 2 )) > "$users_file"; echo "phase $n $mode: $users users, $errors errors, again at $(cat "$users_file")"; continue; fi
            if awk "BEGIN {exit !($(secs "${p95:-0s}") < 1)}"; then echo $(( users * 2 )) > "$users_file"; echo "phase $n $mode: $users users, p95 $p95, not saturated, again at $(cat "$users_file")"; continue; fi
            # served per second over the run: 30 s ramp-up + 2 min hold + 30 s ramp-down
            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date +%H:%M)" "$n" "$mode" "$users" "$p95" "$p99" "$errors" "$served" \
              "$(awk "BEGIN {printf \"%.0f\", $served / 180}")" "$cpu" "$ram" | tee -a "$RESULTS/ladder.tsv"
            break
          done
        done
      done 3< <(phases)
    done
    bash "$HERE/bench.sh" stop
    ;;
  *) sed -n '2,15p' "$0"; exit 1 ;;
esac
