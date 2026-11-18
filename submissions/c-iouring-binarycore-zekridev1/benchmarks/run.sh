#!/usr/bin/env bash
# benchmarks/run.sh: run a reproducible k6 load test against BinaryCore.
#
#   BASE_URL=http://127.0.0.1:3000 VUS=2000 DURATION=60s \
#     bash benchmarks/run.sh
#
# Requires: k6. Records a machine-readable JSON report.
set -euo pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:3000}"
VUS="${VUS:-2000}"
DURATION="${DURATION:-60s}"
RAMP_UP="${RAMP_UP:-30s}"
TOKENS="${TOKENS:-../../seed/tokens.json}"
OUT="${OUT:-bench-result.json}"

echo "== BinaryCore benchmark"
echo "  BASE_URL=$BASE_URL VUS=$VUS DURATION=$DURATION RAMP_UP=$RAMP_UP"
echo "  git commit: $(git -C ../../ rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "  compiler:   $(gcc --version 2>/dev/null | head -1 || echo unknown)"
echo "  kernel:     $(uname -r 2>/dev/null || echo unknown)"

k6 run \
  -e BASE_URL="$BASE_URL" \
  -e VUS="$VUS" \
  -e DURATION="$DURATION" \
  -e RAMP_UP="$RAMP_UP" \
  -e TOKENS="$TOKENS" \
  --summary-export "$OUT" \
  ../../bench/load.js

echo "== report written to $OUT"
