#!/usr/bin/env bash
# Compares two setups in the same session, alternating them (A, B, A, B...) at a fixed number of users, so that the
# machine's drift over hours (background load, heat) hits both alike. Pick a load above both setups' saturation: the
# requests column of results/summary.tsv then measures efficiency (requests served by a saturated server), the
# latencies show how far each setup is past its limit.
# A setup is a label, an image (bench.sh build with IMAGE=...) and extra `docker run` args.
#   bash local-bench/compare.sh <users> <rounds> <label A> <image A> "<args A>" <label B> <image B> "<args B>"
#   bash local-bench/compare.sh 9000 1 c0-classic app:s1 "--tmpfs /app/php.d" s1-classic app:s1 ""
set -uo pipefail
bench="$(cd "$(dirname "$0")" && pwd)/bench.sh"
users="$1"; rounds="$2"
for _ in $(seq "$rounds"); do
  for setup in a b; do
    if [ "$setup" = a ]; then label="$3"; image="$4"; args="$5"; else label="$6"; image="$7"; args="$8"; fi
    IMAGE="$image" SERVE_ARGS="$args" bash "$bench" start >/dev/null
    echo "== $label, $users users"
    bash "$bench" load "$users" "$label" 2>&1 | grep -E "✗|http_req_duration\.|http_req_failed\.|peak|hold CPU|exit code"
  done
done
bash "$bench" stop
