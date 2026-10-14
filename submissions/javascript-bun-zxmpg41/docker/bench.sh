#!/usr/bin/env bash
# Like bench.sh, but k6 runs in a container next to the server (same Docker network).
# Use this for > ~10k VUs locally; the host path is limited by the Mac's bridge and port range.
#   VUS=20000 RAMP_UP=120s DURATION=1m bash docker/bench-docker.sh
# Give the VM room: k6 gets K6_CPUS (default 2) + the server 1 CPU, e.g. colima start --cpu 4 --memory 6
set -euo pipefail
cd "$(dirname "$0")"
bash up.sh
rc=0; docker compose --profile bench run --rm k6 || rc=$?
exit $rc
