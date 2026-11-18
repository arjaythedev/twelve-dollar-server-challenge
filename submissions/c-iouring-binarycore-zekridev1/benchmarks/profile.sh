#!/usr/bin/env bash
# benchmarks/profile.sh: perf profile of the running server.
#
#   sudo perf record -F 99 -a -g -- sleep 30
#   sudo perf report
set -euo pipefail
echo "Run: sudo perf record -F 99 -a -g -- sleep 30  (against a running server)"
echo "Then: sudo perf report"
