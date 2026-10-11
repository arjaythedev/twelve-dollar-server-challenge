#!/usr/bin/env bash
set -euo pipefail
export BAML_TELEMETRY=off TOKIO_WORKER_THREADS=1
exec "$(dirname "$0")/bin/server"
