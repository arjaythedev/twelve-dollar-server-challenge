#!/usr/bin/env bash
# PR 136 (this submission) against PR 40, the other Symfony + FrankenPHP entry, as the leaderboard box sees them: on
# its 1 vCPU, PR 40 served 35% more requests per CPU second (428 µs per request, PR 136 599). At a full core, this
# stand-in box doesn't show it: the server's CPU sits near 50% at any load, both at 15,000 users within 2 points.
# Throttled to a quarter of the core, both saturate and the requests they serve show it (PR 40 +34.6%). About 3.5
# minutes a run.
#   bash local-bench/bench.sh build && bash local-bench/bench.sh build-peer php-symfony-pklebba
#   bash local-bench/compare-pr40.sh [users, default 10000] [rounds, default 2]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
WARMUP=0 DURATION=2m RAMP_UP=30s bash "$HERE/compare.sh" "${1:-10000}" "${2:-2}" \
  t025-pr136 twelve-dollar-php-symfony "--cpus=0.25" \
  t025-pr40 twelve-dollar-php-symfony-pklebba "--cpus=0.25"
