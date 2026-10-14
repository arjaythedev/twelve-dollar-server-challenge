#!/usr/bin/env bash
# Finds the limit like the real score: step up from a passing level (doubling the step) until a run fails, then bisect
# to within 250 users. Each run starts a fresh server and goes through bench.sh load (warm-up, then a 5 min hold).
# The starting level is only a guess (the previous step's limit): if the first try fails, it is checked too, and the
# search steps down from it (doubling the step) until a run passes.
#   bash local-bench/find-limit.sh <label> <expected passing users> [first users to try]
#   SERVE_ARGS="-v $PWD/local-bench/Caddyfile.worker:/app/Caddyfile:ro" bash local-bench/find-limit.sh c0-worker 10000 20000
# MAX caps the search (one k6 container runs out of TCP ports near 64,000 users): passing it ends the search.
set -uo pipefail
bench="$(cd "$(dirname "$0")" && pwd)/bench.sh"
label="$1"; lo="$2"; hi=""; try="${3:-$(( lo * 2 ))}"; step=$(( try - lo )); max="${MAX:-}"
run() {
  bash "$bench" start >/dev/null
  echo "== $label, $1 users"
  bash "$bench" load "$1" "$label" 2>&1 | grep -E "✗|http_req_duration\.|http_req_failed\.|peak|hold CPU|exit code" | tee /dev/stderr | grep -q "exit code: 0 "
}
passed=""
while [ -z "$hi" ]; do
  if run "$try" 2>&1; then
    lo=$try; passed=1
    if [ -n "$max" ] && [ "$lo" -ge "$max" ]; then hi="above MAX"; break; fi
    step=$(( step * 2 )); try=$(( lo + step ))
    if [ -n "$max" ] && [ "$try" -gt "$max" ]; then try=$max; fi
  else
    hi=$try
  fi
done
if [ -z "$passed" ]; then
  until run "$lo" 2>&1; do hi=$lo; lo=$(( lo - step )); step=$(( step * 2 )); done
fi
while [ "$hi" != "above MAX" ] && [ $(( hi - lo )) -gt 250 ]; do
  mid=$(( (lo + hi) / 2 / 250 * 250 ))
  if run "$mid" 2>&1; then lo=$mid; else hi=$mid; fi
done
bash "$bench" stop
echo "== limit: passes $lo, fails $hi"
