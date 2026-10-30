#!/usr/bin/env bash
# Compares setups in the same session, taking turns (A, B, C, A, B, C...) at a fixed number of users, so that the
# machine's drift over hours (background load, heat) hits them all alike. Pick a load above every setup's saturation:
# the requests column of results/summary.tsv then measures efficiency (requests served by a saturated server), the
# latencies show how far each setup is past its limit. Below saturation, compare the hold CPU instead.
# A setup is a label, an image (bench.sh build with IMAGE=..., or bench.sh build-peer) and extra `docker run` args.
#   bash local-bench/compare.sh <users> <rounds> <label A> <image A> "<args A>" <label B> <image B> "<args B>" [...]
#   bash local-bench/compare.sh 9000 1 c0-classic app:s1 "--tmpfs /app/php.d" s1-classic app:s1 ""
set -uo pipefail
bench="$(cd "$(dirname "$0")" && pwd)/bench.sh"
users="$1"; rounds="$2"; shift 2
setups=("$@")
for _ in $(seq "$rounds"); do
  for (( i = 0; i < ${#setups[@]}; i += 3 )); do
    label="${setups[i]}"; image="${setups[i + 1]}"; args="${setups[i + 2]}"
    IMAGE="$image" SERVE_ARGS="$args" bash "$bench" start >/dev/null
    echo "== $label, $users users"
    bash "$bench" load "$users" "$label" 2>&1 | grep -E "✗|http_req_duration\.|http_req_failed\.|peak|hold CPU|exit code"
  done
done
bash "$bench" stop
