#!/usr/bin/env bash
# check153.sh VARIANT [EXPECT...] — correctness suites against one build (benchmark host, root).
# A private network namespace; the server runs as UID 65534 under LimitNOFILE=65535 on a fresh
# seed copy. Suites, in this order (read-only ones first, since they compare with the seed):
#   oracle (20,000 posts + feed vs SQL), recv_stress, freeze_recover (SIGSTOP/SIGCONT with
#   FREEZE_CONNS connections), the official 42 checks (need the fresh database), edge_cases,
#   holds_stress, backpressure, foreign_writer (last: it turns the native plans off).
# EXPECT (STATS / fault builds): recv   — ENOBUFS ends and queued re-arms both happened;
#                                 backlog — SQ backlog entries were used;
#                                 fault  — EBUSY, EAGAIN, ENOMEM, partial and cut-after-shutdown
#                                          submissions were all injected.
# Always: no sanitizer/fatal output, the server still alive, and close_early = 0 in STATS builds.
# Logs and one line per suite go to results/check-VARIANT-<time>/ whatever happens.
set -uo pipefail
R=${STUDY_ROOT:-/root/twelve/lean-153-improve}; V=$1; shift; EXPECT="$*"; C=/root/twelve/challenge
export R V C EXPECT FREEZE_CONNS=${FREEZE_CONNS:-16000}
exec unshare -n bash -c '
set -uo pipefail
ip link set lo up
ulimit -n 1048576
D=$R/results/check-$V-$(date +%s); mkdir -p $D
W=$(mktemp -d /tmp/l153chk-XXXXXX); chown 65534:65534 $W
cp $C/seed/feed.db $W/feed.db; chown 65534:65534 $W/feed.db
cp $R/artifacts/$V/server $W/server; chmod 755 $W/server
prlimit --nofile=65535:65535 setpriv --reuid=65534 --regid=65534 --clear-groups env SQLITE_PATH=$W/feed.db \
  JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=3000 ASAN_OPTIONS=detect_leaks=0 $W/server > $W/server.log 2>&1 &
P=$!
trap "cp $W/server.log $W/*.log $D/ 2>/dev/null; kill $P 2>/dev/null; rm -rf $W" EXIT
for i in $(seq 200); do curl -sf -m 1 http://127.0.0.1:3000/health >/dev/null && break; sleep 0.1; done
rc=0
suite() { local name=$1; shift; "$@" > $W/$name.log 2>&1; local s=$?; echo "$name exit=$s $(tail -1 $W/$name.log)" | tee -a $D/suites.txt; [ $s = 0 ] || rc=1; }
suite oracle python3 $R/harness/oracle.py $C/seed/feed.db
suite recv_stress python3 $R/harness/recv_stress.py $C/seed/feed.db 300 48
suite freeze_recover python3 $R/harness/freeze_recover.py $C/seed/feed.db $P $FREEZE_CONNS
suite official bash $C/test/test.sh http://127.0.0.1:3000
suite edge_cases python3 $R/harness/edge_cases.py
suite holds_stress python3 $R/harness/holds_stress.py $C/seed/feed.db 1500   # baseline counts from the seed (posts/users untouched by earlier suites; the live file may be exclusively locked)
suite backpressure python3 $R/harness/backpressure.py
suite foreign_writer python3 $R/harness/foreign_writer.py $W/feed.db $W/server.log
kill -0 $P 2>/dev/null || { echo "SERVER DIED" | tee -a $D/suites.txt; rc=1; }
# STATS builds print every 10 s from inside the loop: wait, then wake the loop once
if grep -aq "stats: iters" $W/server; then sleep 11; curl -s -o /dev/null http://127.0.0.1:3000/health; sleep 0.5; fi
kill $P; wait $P 2>/dev/null
if grep -E "ERROR|runtime error|AddressSanitizer|fatal|before its shutdown" $W/server.log | head -5 | grep .; then echo "SANITIZER/FATAL OUTPUT" | tee -a $D/suites.txt; rc=1; fi
T=$(grep -E "^stats:" $W/server.log | tail -1 | sed -n "s/.*| total //p")
F=$(grep -E "^fault:" $W/server.log | tail -1)
[ -n "$T" ] && echo "totals: $T" | tee -a $D/suites.txt
[ -n "$F" ] && echo "$F" | tee -a $D/suites.txt
val() { echo "$1" | tr " " "\n" | grep -A1 -x "$2" | tail -1; }
need() { local v; v=$(val "$1" "$2"); if [ -z "$v" ] || [ "$v" -le 0 ]; then echo "EXPECTATION FAILED: $2 = ${v:-missing}" | tee -a $D/suites.txt; rc=1; fi; }
if [ -n "$T" ]; then e=$(val "$T" close_early); [ "${e:-1}" = 0 ] || { echo "EXPECTATION FAILED: close_early=$e" | tee -a $D/suites.txt; rc=1; }; fi
for x in $EXPECT; do case $x in
  recv) need "$T" enobufs; need "$T" rearms ;;
  backlog) need "$T" backlog ;;
  fault) for k in ebusy eagain enomem partial cut_after_shutdown; do need "${F#fault: }" $k; done ;;
esac; done
echo "CHECK $V rc=$rc logs=$D" | tee -a $D/suites.txt
exit $rc'
