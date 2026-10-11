#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
# Keep automatic telemetry local: no cloud service participates in the benchmark.
export BOUNDARY_API_KEY=local
exec ./bin/server
