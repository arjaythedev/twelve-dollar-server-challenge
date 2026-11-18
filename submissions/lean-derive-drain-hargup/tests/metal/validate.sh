#!/usr/bin/env bash
# t7.sh SRC_DIR TAG [FEATURE] — the validation matrix for a candidate (as t5/t6), with coverage assertions
# split by what each test build really exercises: the 4-entry SQ build must use the backlog, the
# tight build (CQE budget 4, NBUF 4, MAX_JOBS 64) the recovery path.
cd /root/twelve/lean-153-improve; S=$1; T=$2; X=${3:-}   # X: a build feature added to every build (e.g. excl)
mode() { if [ -z "$X" ]; then echo "$1"; elif [ "$1" = normal ]; then echo "$X"; else echo "$1+$X"; fi; }
gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I$S/runtime -I$S/gen $S/tests/metal/transport_cases.c -o /tmp/l153-tc-$T 2>/dev/null
for m in normal stats stats+asan nbuf4+stats+asan sq4+stats+asan fault+nbuf4+sq8+stats+asan tight+stats+asan fault+stats; do
  flock -w 3600 /root/twelve/bench.lock bash $S/tests/metal/build_remote.sh $S $T-$m $(mode $m) 2>&1 | tail -1
done
echo "== unit $(/tmp/l153-tc-$T 2>/dev/null | tail -1)"
run() { echo "== check $1 [$2] $(date -u +%T)"; flock -w 3600 /root/twelve/bench.lock timeout 900 bash harness/check153.sh $1 $2 2>&1 | grep -E "exit=|totals|fault:|CHECK|EXPECT|SANITIZER|DIED"; }
run $T-normal ""
run $T-stats "recv"
run $T-stats+asan "recv"
run $T-nbuf4+stats+asan "recv"
run $T-sq4+stats+asan "backlog"
run $T-fault+nbuf4+sq8+stats+asan "recv backlog fault"
run $T-tight+stats+asan "recv"
run $T-fault+stats "fault recv"
echo "== crash $(date -u +%T)"; flock -w 3600 /root/twelve/bench.lock bash harness/crash153.sh $T-normal 20 | tail -2
echo T7_DONE $(date -u +%T)
