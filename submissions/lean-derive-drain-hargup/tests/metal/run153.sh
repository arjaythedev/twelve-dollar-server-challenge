#!/usr/bin/env bash
# run153.sh VARIANT [bench153.py options] — wait for a quiet host (no other load generator or
# compiler), hold the shared benchmark lock, run one measurement in a private network namespace.
set -euo pipefail
R=${STUDY_ROOT:-/root/twelve/lean-153-improve}
exec flock -w 7200 /root/twelve/bench.lock bash -c '
  python3 "'"$R"'/harness/wait_quiet.py" 10
  exec unshare -n python3 "'"$R"'/harness/bench153.py" "$@"' bash "$@"
