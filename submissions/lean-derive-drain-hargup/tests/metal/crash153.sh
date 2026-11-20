#!/usr/bin/env bash
# crash153.sh VARIANT [ROUNDS] — kill -9 under pipelined writes, then every acknowledged write
# must be in the recovered database and integrity_check must pass (crash_durability.py, from the
# low-memory branch). Private network namespace; logs to results/crash-VARIANT-<time>.
set -uo pipefail
R=${STUDY_ROOT:-/root/twelve/lean-153-improve}; V=$1; N=${2:-20}
D=$R/results/crash-$V-$(date +%s); mkdir -p $D
unshare -n bash -c "ip link set lo up; ulimit -n 65535; PORT=3100 python3 $R/harness/crash_durability.py $R/artifacts/$V/server /root/twelve/challenge/seed/feed.db $N" 2>&1 | tee $D/crash.log | tail -3
exit ${PIPESTATUS[0]}
