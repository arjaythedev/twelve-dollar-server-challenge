#!/usr/bin/env bash
# campaign_hold.sh CAND "LEVELS" [ROUNDS] [KERNEL] — matched 5-minute holds in the 1-vCPU / 2 GB guest,
# as the official search confirms a score: at each level, the candidate, #264 and the original #153,
# interleaved (order reversed every round). The official verdict is p95 < 500 ms, p99 < 1 s (k6
# http_req_duration over the whole run) and < 1 % errors; vmsum.py prints it (column k6).
set -uo pipefail
cd /root/twelve/lean-153-improve
CAND=${1:?candidate tag}; LEVELS=${2:?levels}; ROUNDS=${3:-2}; K=${4:-6.8.0-100-generic}
L=leader-4298101; B=base-fd2c149
flock -w 7200 /root/twelve/bench.lock bash harness/vm_build.sh $CAND $L $B submit-861eac8 2>&1 | tail -1
run() { echo "== $* $(date -u +%T)"; bash harness/runvm153.sh "$@" > logs/vm-last.out 2>&1; grep "^{" logs/vm-last.out | cut -c1-3000 || { echo "RUN FAILED"; tail -5 logs/vm-last.out; }; }
for vus in $LEVELS; do
  V="$CAND $L $B"
  for r in $(seq $ROUNDS); do
    for v in $V; do run $v --vus $vus --ramp 60 --duration 300 --rampdown 30 --kernel $K --tag hold-$((vus/1000)); done
    V=$(echo $V | tr ' ' '\n' | tac | tr '\n' ' ')
  done
done
echo CAMPAIGN_HOLD_DONE $(date -u +%FT%TZ)
