#!/usr/bin/env bash
# The limits: each phase's score, found like the real one (the repo's README, "Scoring"), on the ladder's images and
# throttled like the ladder (0.25 CPU, for the box's slower core). Each run is the real score's: a fresh server, the
# 1,000-user warm-up, then bench/load.js (60 s ramp-up, 5 min hold, 30 s ramp-down), passing with p95 < 500 ms,
# p99 < 1 s and errors < 1%. The search narrows down to within 250 users, then one more run confirms the limit,
# stepping down 250 users until one passes. Only its start differs: not 2,500 users doubling, but a guess from the
# setup's requests per second in results/ladder.tsv (about 9 users each), stepping 250 users from it, doubling.
#
#   bash local-bench/limits.sh dry                     # a run past the limit of the lightest and heaviest setups
#   bash local-bench/limits.sh run [<phase> <mode>]... # every setup of results/ladder.tsv, or those given
#   GUESS=6000 bash local-bench/limits.sh run php-symfony-pklebba peer
#                                                      # another submission (bench.sh build-peer), from a guess
# Env: RUNS and LIMITS, other files than results/limits-runs.tsv and results/limits.tsv (e.g. for a second search).
#
# Each run is one row of results/limits-runs.tsv (p95, p99, errors, hold CPU, peak RAM); each setup adds two rows to
# results/limits.tsv: its score (the confirming run) and the failing run 250 users above. Runs and setups already
# there are reused: after an interruption, run it again to resume. Anything other than a pass or a failed threshold
# (no server, no k6 results, a server that stops answering after a run) stops everything.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
RESULTS="$HERE/results"
STATE="$RESULTS/ladder"
GRAIN=250
export WARMUP=1 RAMP_UP=60s DURATION=5m
RUNS="${RUNS:-$RESULTS/limits-runs.tsv}"
LIMITS="${LIMITS:-$RESULTS/limits.tsv}"
[ -f "$RUNS" ] || printf 'time\tphase\tmode\tusers\tp95\tp99\terrors\thold_cpu\tpeak_ram\tresult\tminutes\trun\n' > "$RUNS"
[ -f "$LIMITS" ] || head -1 "$RUNS" > "$LIMITS"

healthy() {
  for _ in $(seq 12); do curl -sf --max-time 5 http://127.0.0.1:3000/health >/dev/null && return 0; sleep 5; done
  return 1
}
# the row of a run already done: <phase> <mode> <users> <search|confirm|any>
row() {
  awk -F'\t' -v n="$1" -v m="$2" -v u="$3" -v k="$4" '$2 == n && $3 == m && $4 == u && (k == "any" || $12 == k) {r = $0} END {print r}' "$RUNS"
}

# one run of a search or a confirmation at <phase> <mode> <users>: 0 when it passes, 1 when a threshold fails,
# exits on anything else
attempt() {
  local n="$1" mode="$2" users="$3" kind="$4" args="--cpus=0.25" image out started p95 p99 errors cpu ram code result
  # a search takes about 8 steps: many more is a search going round in circles
  STEPS=$(( STEPS + 1 ))
  [ "$STEPS" -le 20 ] || { echo "phase $n $mode: still searching after 20 steps" >&2; exit 1; }
  result="$(row "$n" "$mode" "$users" "$kind" | cut -f10)"
  if [ -n "$result" ]; then echo "phase $n $mode, $users users ($kind): $result (already run)"; [ "$result" = pass ]; return; fi
  [ "$mode" = classic ] && args="$args -v $STATE/Caddyfile.classic.p$n:/app/Caddyfile:ro"
  image="twelve-dollar-php-symfony:ladder-p$n"; [ "$mode" = peer ] && image="twelve-dollar-$n"
  IMAGE="$image" SERVE_ARGS="$args" bash "$HERE/bench.sh" start >/dev/null
  healthy || { echo "phase $n $mode: the server didn't start" >&2; exit 1; }
  started=$SECONDS
  out="$(bash "$HERE/bench.sh" load "$users" "limits-p$n-$mode" 2>&1)"
  p95="$(grep -E 'http_req_duration\.' <<< "$out" | grep -Eo 'p\(95\)=[^ ]+' | cut -d= -f2)"
  p99="$(grep -E 'http_req_duration\.' <<< "$out" | grep -Eo 'p\(99\)=[^ ]+' | cut -d= -f2)"
  errors="$(grep -E 'http_req_failed\.' <<< "$out" | awk '{print $2}')"
  cpu="$(grep -Eo 'hold CPU \(median\): [0-9.]+%' <<< "$out" | grep -Eo '[0-9.]+%')"
  ram="$(grep -Eo '^peak CPU: .*peak RAM: [0-9.]+[KMG]iB' <<< "$out" | grep -Eo 'peak RAM: [0-9.]+[KMG]iB' | awk '{print $3}')"
  code="$(grep -Eo 'k6 exit code: [0-9]+' <<< "$out" | awk '{print $4}')"
  if [ -z "$p95" ] || [ -z "$errors" ]; then echo "phase $n $mode, $users users: no k6 results, see results/*-limits-p$n-$mode-$users.k6" >&2; exit 1; fi
  # 99: thresholds crossed; any other non-zero code is k6 itself failing
  case "$code" in 0) result=pass ;; 99) result=FAIL ;; *) echo "phase $n $mode, $users users: k6 exit code $code" >&2; exit 1 ;; esac
  healthy || { echo "phase $n $mode, $users users: the server stopped answering after the run" >&2; exit 1; }
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date +%H:%M)" "$n" "$mode" "$users" "$p95" "$p99" "$errors" "$cpu" "$ram" \
    "$result" "$(( (SECONDS - started + 30) / 60 ))" "$kind" | tee -a "$RUNS"
  [ "$result" = pass ]
}

search() {
  local n="$1" mode="$2" rate lo=0 hi="" next step=$GRAIN score fail
  STEPS=0
  if awk -F'\t' -v n="$n" -v m="$mode" '$2 == n && $3 == m {f = 1} END {exit !f}' "$LIMITS"; then return; fi
  if [ "$mode" = peer ]; then
    next="${GUESS:?GUESS: the users to start the search of $n from}"
  else
    rate="$(awk -F'\t' -v n="$n" -v m="$mode" '$2 == n && $3 == m {s += $9; c++} END {if (c) printf "%.0f", s / c}' "$RESULTS/ladder.tsv")"
    [ -n "$rate" ] || { echo "phase $n $mode: not in results/ladder.tsv" >&2; exit 1; }
    next=$(( rate * 9 ))
  fi
  next=$(( next / GRAIN * GRAIN )); [ "$next" -ge "$GRAIN" ] || next=$GRAIN
  if attempt "$n" "$mode" "$next" search; then
    lo=$next
    until [ -n "$hi" ]; do
      next=$(( lo + step )); step=$(( step * 2 ))
      if attempt "$n" "$mode" "$next" search; then lo=$next; else hi=$next; fi
    done
  else
    hi=$next
    while [ "$lo" = 0 ] && [ "$hi" -gt "$GRAIN" ]; do
      next=$(( hi - step )); step=$(( step * 2 )); [ "$next" -ge "$GRAIN" ] || next=$GRAIN
      if attempt "$n" "$mode" "$next" search; then lo=$next; else hi=$next; fi
    done
  fi
  while [ $(( hi - lo )) -gt "$GRAIN" ]; do
    next=$(( (lo + hi) / 2 / GRAIN * GRAIN ))
    if attempt "$n" "$mode" "$next" search; then lo=$next; else hi=$next; fi
  done
  score=$lo
  while [ "$score" -gt 0 ] && ! attempt "$n" "$mode" "$score" confirm; do score=$(( score - GRAIN )); done
  # the score and the failing run 250 users above (a failed confirmation, or else the search's): what gave out
  fail="$(row "$n" "$mode" $(( score + GRAIN )) confirm)"
  [ -n "$fail" ] || fail="$(row "$n" "$mode" $(( score + GRAIN )) search)"
  { if [ "$score" -gt 0 ]; then row "$n" "$mode" "$score" confirm; else printf '\t%s\t%s\t0\n' "$n" "$mode"; fi; echo "$fail"; } | tee -a "$LIMITS"
}

case "${1:-}" in
  dry)
    # past the limit, where a server could hang: twice the guess of the lightest setup, and of the heaviest
    STEPS=0
    attempt 0 classic 750 search
    last="$(awk -F'\t' 'NR > 1 {n = $2} END {print n}' "$RESULTS/ladder.tsv")"
    attempt "$last" worker 12000 search
    bash "$HERE/bench.sh" stop
    ;;
  run)
    shift
    if [ $# -gt 0 ]; then setups="$(printf '%s %s\n' "$@")"
    else setups="$(awk -F'\t' 'NR > 1 && !seen[$2 " " $3]++ {print $2, $3}' "$RESULTS/ladder.tsv")"; fi
    while read -r n mode <&3; do search "$n" "$mode"; done 3<<< "$setups"
    bash "$HERE/bench.sh" stop
    ;;
  *) sed -n '2,18p' "$0"; exit 1 ;;
esac
