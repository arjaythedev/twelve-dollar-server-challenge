#!/usr/bin/env bash
# Restart on a fresh DB and run the official test suite against the container.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
bash "$HERE/up.sh"
bash "$HERE/../../../test/test.sh" "http://127.0.0.1:${HOST_PORT:-8080}"
